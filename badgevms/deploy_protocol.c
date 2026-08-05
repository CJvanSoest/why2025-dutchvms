/* BadgeVMS deploy protocol — Phase B.
 *
 * Wire format:
 *   Host -> Badge: [MAGIC: DE AD BE EF][CMD:1][LEN:4 LE][PAYLOAD:N][CRC16:2 LE]
 *   Badge -> Host: [MAGIC: DE AD C0 DE][STATUS:1][LEN:4 LE][PAYLOAD:N][CRC16:2 LE]
 *
 * CRC: esp_rom_crc16_le over [CMD/STATUS:1][LEN:4][PAYLOAD:N], initial 0xFFFF.
 *
 * Commands:
 *   0x01 PUT   payload = [path_len:2 LE][path:N][file_data:M]
 *              response payload = [bytes_written:4 LE]
 *              streamed straight to a ".part" temp file + rename() on
 *              success (task #44) -- every other command's (always-small)
 *              payload is still buffered whole in one malloc() first
 *   0x02 GET   payload = [path_len:2 LE][path:N]
 *              response payload = raw file bytes
 *   0x03 LIST  payload = [path_len:2 LE][path:N]
 *              response payload = UTF-8 text, one "<name>\t<size>\t<D|F>\n"
 *              line per directory entry (truncated, not erred, if the
 *              directory doesn't fit LIST_BUFFER_BYTES)
 *   0x04 DELETE payload = [path_len:2 LE][path:N]
 *              removes a single file, or a directory and everything under
 *              it (recursive). response payload empty. Meant for surgical
 *              cleanup of individual stale files/app-dirs (e.g. on FLASH0,
 *              which a full storage.bin reflash doesn't reliably clear) -
 *              not a bulk wipe primitive.
 *   0x07 PING  payload empty, response payload = ASCII version string
 *
 * Status codes (response byte):
 *   0x00 OK
 *   0x01 ERR_BAD_FRAME    (bad CRC or unparseable)
 *   0x02 ERR_OOM
 *   0x03 ERR_BAD_PATH
 *   0x04 ERR_FOPEN
 *   0x05 ERR_WRITE
 *   0x06 ERR_UNKNOWN_CMD
 *   0x07 ERR_TOO_BIG
 *   0x08 ERR_READ
 *
 * Logs from kernel tasks use esp_rom_printf (ESP_LOG crashes the task on
 * BadgeVMS' picolibc setup even at 4096 stack — see project memory).
 */

#include "deploy_protocol.h"

#include "esp_rom_crc.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"
#include "rom/uart.h"
#include "task.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* We deliberately avoid why_fopen / why_open here — those route through
 * BadgeVMS' wrapped_funcs which dereference per-task thread context
 * (task_info->thread->file_handles[]). Kernel tasks created via
 * create_kernel_task do not have a thread struct, so calling why_fopen
 * from one crashes the task with a NULL deref (MEPC=0).
 *
 * Instead, we convert VMS paths (SD0:[BADGEVMS.APPS.foo]bar.ext) to the
 * POSIX mount-point paths that fatfs.c registered with ESP-IDF VFS
 * (/SD0/BADGEVMS/APPS/foo/bar.ext) and call standard open()/write()/close()
 * which resolve through picolibc → ESP-IDF VFS → FATFS without touching
 * BadgeVMS' thread state. */

#define MAX_PAYLOAD_BYTES (2 * 1024 * 1024) /* 2 MB cap, PUT/GET file transfers */
#define MAX_PATH_BYTES    256
#define LIST_BUFFER_BYTES (16 * 1024) /* directory listing text, not a file transfer */

#define CMD_PUT    0x01
#define CMD_GET    0x02
#define CMD_LIST   0x03
#define CMD_DELETE 0x04
#define CMD_PING   0x07

#define ST_OK              0x00
#define ST_ERR_BAD_FRAME   0x01
#define ST_ERR_OOM         0x02
#define ST_ERR_BAD_PATH    0x03
#define ST_ERR_FOPEN       0x04
#define ST_ERR_WRITE       0x05
#define ST_ERR_UNKNOWN_CMD 0x06
#define ST_ERR_TOO_BIG     0x07
#define ST_ERR_READ        0x08

static uint8_t const REQ_MAGIC[4]  = {0xDE, 0xAD, 0xBE, 0xEF};
static uint8_t const RESP_MAGIC[4] = {0xDE, 0xAD, 0xC0, 0xDE};

static TaskHandle_t deploy_handle = NULL;

/* ============== UART I/O helpers ============== */

static void rx_blocking(uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        while (1) {
            ETS_STATUS s = uart_rx_one_char(&buf[i]);
            if (s == ETS_OK)
                break;
            vTaskDelay(2 / portTICK_PERIOD_MS);
        }
    }
}

static void tx_bytes(uint8_t const *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uart_tx_one_char(buf[i]);
    }
}

static void send_response(uint8_t status, uint8_t const *payload, uint32_t len) {
    uint8_t header[5];
    header[0] = status;
    header[1] = (uint8_t)(len & 0xFF);
    header[2] = (uint8_t)((len >> 8) & 0xFF);
    header[3] = (uint8_t)((len >> 16) & 0xFF);
    header[4] = (uint8_t)((len >> 24) & 0xFF);

    /* CRC over [status:1][len:4][payload:N] */
    uint16_t crc = 0xFFFF;
    crc          = esp_rom_crc16_le(crc, header, 5);
    if (payload && len)
        crc = esp_rom_crc16_le(crc, payload, len);

    tx_bytes(RESP_MAGIC, 4);
    tx_bytes(header, 5);
    if (payload && len)
        tx_bytes(payload, len);
    uint8_t crc_le[2] = {(uint8_t)(crc & 0xFF), (uint8_t)((crc >> 8) & 0xFF)};
    tx_bytes(crc_le, 2);
}

static void send_status(uint8_t status) {
    send_response(status, NULL, 0);
}

/* ============== Magic-byte scanner ============== */

static void scan_for_magic(void) {
    uint8_t window[4] = {0};
    while (1) {
        uint8_t c;
        rx_blocking(&c, 1);
        window[0] = window[1];
        window[1] = window[2];
        window[2] = window[3];
        window[3] = c;
        if (memcmp(window, REQ_MAGIC, 4) == 0)
            return;
    }
}

/* ============== Command handlers ============== */

static void handle_ping(uint8_t const *payload, uint32_t len) {
    (void)payload;
    (void)len;
    static char const VERSION[] = "DutchVMS deploy-proto v0.1\0";
    send_response(ST_OK, (uint8_t const *)VERSION, sizeof(VERSION) - 1);
    esp_rom_printf("[deploy] PING -> OK\n");
}

/* Convert a VMS path like "SD0:[BADGEVMS.APPS.cj_hello]bin.elf" to a
 * POSIX path like "/SD0/BADGEVMS/APPS/cj_hello/bin.elf". Returns 0 on
 * success, -1 on malformed input. */
static int vms_to_posix(char const *vms, char *out, size_t out_size) {
    if (out_size < 2)
        return -1;
    char const *colon = strchr(vms, ':');
    if (!colon || colon == vms)
        return -1;

    size_t pos = 0;
    out[pos++] = '/';
    /* device name */
    for (char const *p = vms; p < colon; p++) {
        if (pos >= out_size - 1)
            return -1;
        out[pos++] = *p;
    }

    char const *rest  = colon + 1;
    char const *lb    = strchr(rest, '[');
    char const *rb    = lb ? strchr(lb, ']') : NULL;
    char const *fname = rest;
    if (lb && rb && lb == rest) {
        /* [dir.subdir] block — replace dots with / */
        if (pos >= out_size - 1)
            return -1;
        out[pos++] = '/';
        for (char const *p = lb + 1; p < rb; p++) {
            if (pos >= out_size - 1)
                return -1;
            out[pos++] = (*p == '.') ? '/' : *p;
        }
        fname = rb + 1;
    }
    /* filename portion (may be empty for directory-only paths) */
    if (*fname) {
        if (pos >= out_size - 1)
            return -1;
        out[pos++] = '/';
        for (char const *p = fname; *p; p++) {
            if (pos >= out_size - 1)
                return -1;
            out[pos++] = *p;
        }
    }
    out[pos] = 0;

    /* Reject '.' / '..' path segments anywhere in the result, so PUT/GET/
     * LIST/DELETE (incl. recursive delete) can never escape the intended
     * SD0:/FLASH0: root via a crafted VMS path. */
    char const *seg = out;
    while (*seg == '/') seg++;
    while (*seg) {
        char const *seg_end = strchr(seg, '/');
        size_t      seg_len = seg_end ? (size_t)(seg_end - seg) : strlen(seg);
        if ((seg_len == 1 && seg[0] == '.') || (seg_len == 2 && seg[0] == '.' && seg[1] == '.'))
            return -1;
        if (!seg_end)
            break;
        seg = seg_end + 1;
        while (*seg == '/') seg++;
    }

    return 0;
}

#define PUT_STREAM_CHUNK_BYTES 2048
/* Kernel task, small stack (see the 6144 comment on create_kernel_task()
 * below) -- a 2KB scratch buffer lives in BSS, not on the stack. Single
 * dedicated deploy_listener_task processes one frame at a time, so an
 * unguarded file-scope static is fine here (same assumption the rest of
 * this file already makes). */
static uint8_t put_stream_chunk[PUT_STREAM_CHUNK_BYTES];

/* rx_blocking() talks straight to the UART ROM driver (uart_rx_one_char()),
 * not the ESP-IDF UART driver's own interrupt-fed ring buffer -- there is
 * essentially nothing but the small hardware RX FIFO backing it. A first
 * cut of streaming PUT (writing each chunk to SD synchronously, inline,
 * between rx_blocking() calls) reproduced the exact "large PUT corrupts in
 * transit" failure this project had already hit once before with a 155KB
 * PUT (see project memory) -- a slow SD/FATFS write() call blocks the
 * reader for long enough that the hardware FIFO overflows and drops bytes
 * before the next rx_blocking() call can drain it, which then fails the
 * end-to-end CRC.
 *
 * Fix: split into a reader (this task, draining UART into a stream buffer,
 * never blocked on anything but incoming bytes) and a writer (a second
 * task, draining that stream buffer to SD independently). The two only
 * touch each other through the stream buffer, so a slow SD write only ever
 * delays the WRITER, never the reader's ability to keep pulling bytes off
 * UART. 32KB of slack comfortably absorbs ordinary SD write latency spikes
 * (FAT cluster allocation, etc.) at the ~11.5KB/s an incoming 115200-baud
 * transfer can ever deliver -- NOT a hard guarantee against overflow (only
 * switching this UART to the ESP-IDF driver's own interrupt-driven ring
 * buffer would be that, a bigger change this wasn't extended to since the
 * same UART is shared with esp_rom_printf console logging elsewhere -- see
 * this file's own top-of-file comment), but a large, empirically-motivated
 * safety margin over the single-chunk exposure this used to have. */
#define PUT_RING_BUFFER_BYTES  (32 * 1024)
#define PUT_WRITER_CHUNK_BYTES 1024
#define PUT_WRITER_STACK_WORDS 3072
#define PUT_WRITER_PRIORITY    3 /* same tier as deploy_listener_task itself */

typedef struct {
    StreamBufferHandle_t stream;
    int                  fd;
    uint32_t             expected_bytes;
    volatile bool        write_failed;
    SemaphoreHandle_t    finished;
} put_writer_ctx_t;

/* No esp_rom_printf() anywhere in this task's per-chunk path, on purpose:
 * console logging shares the same UART this whole protocol runs on (see
 * this file's top-of-file comment), and a debug-instrumented version of
 * this exact function that DID log every chunk reproduced the exact
 * FIFO-overflow/dropped-bytes failure this rewrite exists to fix -- just
 * via esp_rom_printf() calls racing the reader's rx_blocking() instead of
 * SD write() calls doing it. Keep it that way; if this needs debugging
 * again, log a handful of checkpoints (task start/end), never per-chunk. */
static void put_writer_task(void *arg) {
    put_writer_ctx_t *ctx = (put_writer_ctx_t *)arg;
    static uint8_t     writer_buf[PUT_WRITER_CHUNK_BYTES];
    uint32_t            remaining = ctx->expected_bytes;

    while (remaining > 0) {
        size_t want = remaining < sizeof(writer_buf) ? remaining : sizeof(writer_buf);
        size_t got  = xStreamBufferReceive(ctx->stream, writer_buf, want, portMAX_DELAY);
        if (got == 0)
            continue; /* portMAX_DELAY: only happens if the stream buffer itself was reset/deleted */

        if (!ctx->write_failed) {
            size_t written = 0;
            while (written < got) {
                ssize_t w = write(ctx->fd, writer_buf + written, got - written);
                if (w <= 0) {
                    ctx->write_failed = true;
                    break;
                }
                written += (size_t)w;
            }
        }
        remaining -= (uint32_t)got;
    }

    xSemaphoreGive(ctx->finished);
    vTaskDelete(NULL);
}

/* Reads and CRC-folds `n` bytes off the wire without writing them anywhere
 * -- used by handle_put_streaming()'s error paths to stay frame-aligned
 * for the next scan_for_magic() even when there's nothing valid to do with
 * the bytes (bad path, etc). */
static void put_drain(uint32_t n, uint16_t *crc_inout) {
    uint16_t crc = *crc_inout;
    while (n > 0) {
        uint32_t chunk_n = n < PUT_STREAM_CHUNK_BYTES ? n : PUT_STREAM_CHUNK_BYTES;
        rx_blocking(put_stream_chunk, chunk_n);
        crc = esp_rom_crc16_le(crc, put_stream_chunk, chunk_n);
        n   -= chunk_n;
    }
    *crc_inout = crc;
}

/* CMD_PUT, streamed straight to a ".part" temp file and rename()d into
 * place on success -- task #44: a single malloc() sized to the WHOLE frame
 * (what process_one_frame() used to do for every command, PUT included) is
 * what OOM'd here on anything much past ~100KB; a PAX-linked app ELF
 * (~180KB) was the first real thing to hit it. Every other command's
 * payload is at most a VMS path (<256 bytes) and stays on the old buffered
 * path in process_one_frame() -- only PUT needed this.
 *
 * `crc` in already covers the 5-byte header (folded by the caller); this
 * folds in path_len + path + file data as they're read, exactly like the
 * old code did over one buffer, just incrementally. The trailing CRC is
 * checked LAST, after everything is drained, and takes precedence over
 * every other error the same way process_one_frame()'s old up-front check
 * did for every command -- a corrupt frame reports ERR_BAD_FRAME even if
 * it also happens to have e.g. a bad path, and the real destination file
 * is never touched on any error path (only ever the .part temp is). */
static void handle_put_streaming(uint32_t len, uint16_t crc) {
    if (len < 2) {
        put_drain(len, &crc);
        uint8_t crc_bytes[2];
        rx_blocking(crc_bytes, 2);
        send_status(ST_ERR_BAD_FRAME);
        esp_rom_printf("[deploy] PUT: len<2\n");
        return;
    }

    uint8_t path_len_bytes[2];
    rx_blocking(path_len_bytes, 2);
    crc = esp_rom_crc16_le(crc, path_len_bytes, 2);
    uint16_t path_len             = (uint16_t)path_len_bytes[0] | ((uint16_t)path_len_bytes[1] << 8);
    uint32_t after_path_len_field = len - 2;

    bool bad_path_len = (path_len == 0 || path_len >= MAX_PATH_BYTES || (uint32_t)path_len > after_path_len_field);

    char     vms_path[MAX_PATH_BYTES];
    uint32_t data_len = 0;
    if (bad_path_len) {
        put_drain(after_path_len_field, &crc);
        vms_path[0] = 0;
    } else {
        rx_blocking((uint8_t *)vms_path, path_len);
        crc          = esp_rom_crc16_le(crc, (uint8_t const *)vms_path, path_len);
        vms_path[path_len] = 0;
        data_len     = after_path_len_field - path_len;
    }

    char posix_path[MAX_PATH_BYTES + 32];
    bool bad_vms_path = bad_path_len || vms_to_posix(vms_path, posix_path, sizeof(posix_path)) != 0;

    char tmp_path[MAX_PATH_BYTES + 32 + 8];
    int  fd = -1;
    if (!bad_vms_path) {
        /* mkdir -p: create any missing parent directories. We mutate
         * posix_path temporarily by null-terminating at each '/' and
         * calling mkdir. */
        for (char *p = posix_path + 1; *p; p++) {
            if (*p == '/') {
                *p = 0;
                mkdir(posix_path, 0755); /* ignore errors (EEXIST is fine) */
                *p = '/';
            }
        }
        snprintf(tmp_path, sizeof(tmp_path), "%s.part", posix_path);
        fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    }

    /* Writer task setup -- only worth it (and only possible) if there's an
     * open destination fd and actual file bytes coming. On bad_vms_path or
     * fd<0, `remaining` below still has to be drained off UART for CRC/
     * frame-sync, there's just nowhere to write it, so have_writer stays
     * false and the loop below folds CRC only. Same for OOM setting up the
     * stream buffer/semaphore/task itself (should be exceedingly rare --
     * 32KB plus two small kernel objects -- but if it happens, degrade to
     * "drain and report OOM" rather than fall back to the FIFO-risky
     * synchronous write path this replaced). */
    StreamBufferHandle_t stream        = NULL;
    SemaphoreHandle_t    finished      = NULL;
    put_writer_ctx_t     writer_ctx    = {0};
    TaskHandle_t          writer_task_h = NULL;
    bool                  writer_oom    = false;

    if (fd >= 0 && data_len > 0) {
        stream   = xStreamBufferCreate(PUT_RING_BUFFER_BYTES, 1);
        finished = xSemaphoreCreateBinary();
        if (stream && finished) {
            writer_ctx.stream         = stream;
            writer_ctx.fd             = fd;
            writer_ctx.expected_bytes = data_len;
            writer_ctx.write_failed   = false;
            writer_ctx.finished       = finished;
            if (create_kernel_task(
                    put_writer_task,
                    "put_writer",
                    PUT_WRITER_STACK_WORDS,
                    &writer_ctx,
                    PUT_WRITER_PRIORITY,
                    &writer_task_h,
                    0
                ) != pdTRUE) {
                writer_task_h = NULL;
            }
        }
        if (!writer_task_h) {
            writer_oom = true;
            if (stream) {
                vStreamBufferDelete(stream);
                stream = NULL;
            }
            if (finished) {
                vSemaphoreDelete(finished);
                finished = NULL;
            }
        }
    }
    bool have_writer = (writer_task_h != NULL);

    uint32_t remaining = data_len;
    while (remaining > 0) {
        uint32_t n = remaining < PUT_STREAM_CHUNK_BYTES ? remaining : PUT_STREAM_CHUNK_BYTES;
        rx_blocking(put_stream_chunk, n);
        crc = esp_rom_crc16_le(crc, put_stream_chunk, n);
        if (have_writer) {
            /* Reader never blocks on SD I/O itself -- only ever on the
             * writer falling more than PUT_RING_BUFFER_BYTES behind, which
             * the writer's own independent scheduling makes brief even
             * when it happens (see the big comment above put_writer_task).
             * Deliberately no esp_rom_printf() anywhere in this loop or the
             * writer's (see PATCHES.md-style note above put_writer_task) --
             * console output shares this same UART, and logging on every
             * chunk from either task reproduced the exact FIFO-overflow
             * problem this whole rewrite exists to avoid, just via prints
             * instead of SD writes. */
            xStreamBufferSend(stream, put_stream_chunk, n, portMAX_DELAY);
        }
        remaining -= n;
    }

    bool write_fail = writer_oom;
    if (have_writer) {
        xSemaphoreTake(finished, portMAX_DELAY);
        write_fail = writer_ctx.write_failed;
        vSemaphoreDelete(finished);
        vStreamBufferDelete(stream);
    }

    uint8_t crc_bytes[2];
    rx_blocking(crc_bytes, 2);
    uint16_t crc_wire = (uint16_t)crc_bytes[0] | ((uint16_t)crc_bytes[1] << 8);

    if (fd >= 0)
        close(fd);

    if (crc != crc_wire) {
        if (fd >= 0)
            unlink(tmp_path);
        send_status(ST_ERR_BAD_FRAME);
        esp_rom_printf("[deploy] PUT CRC mismatch: wire=0x%04X calc=0x%04X\n", crc_wire, crc);
        return;
    }
    if (bad_vms_path) {
        send_status(ST_ERR_BAD_PATH);
        esp_rom_printf("[deploy] PUT: bad path (path_len=%u)\n", (unsigned)path_len);
        return;
    }
    if (fd < 0) {
        send_status(ST_ERR_FOPEN);
        esp_rom_printf("[deploy] PUT open failed for '%s.part' (errno=%d)\n", posix_path, errno);
        return;
    }
    if (writer_oom) {
        unlink(tmp_path);
        send_status(ST_ERR_OOM);
        esp_rom_printf("[deploy] PUT: OOM setting up writer task/stream buffer for '%s'\n", posix_path);
        return;
    }
    if (write_fail) {
        unlink(tmp_path);
        send_status(ST_ERR_WRITE);
        esp_rom_printf("[deploy] PUT write failed for '%s' (errno=%d)\n", posix_path, errno);
        return;
    }
    if (rename(tmp_path, posix_path) != 0) {
        unlink(tmp_path);
        send_status(ST_ERR_WRITE);
        esp_rom_printf("[deploy] PUT rename failed for '%s' (errno=%d)\n", posix_path, errno);
        return;
    }

    esp_rom_printf("[deploy] PUT '%s' -> '%s' %u bytes OK (streamed)\n", vms_path, posix_path, (unsigned)data_len);
    uint8_t reply[4] = {
        (uint8_t)(data_len & 0xFF),
        (uint8_t)((data_len >> 8) & 0xFF),
        (uint8_t)((data_len >> 16) & 0xFF),
        (uint8_t)((data_len >> 24) & 0xFF),
    };
    send_response(ST_OK, reply, 4);
}

/* Shared path-decode step for GET/LIST: both take payload = [path_len:2 LE][path:N]
 * with no trailing data. Returns 0 and fills out_posix on success, sends an
 * error response and returns -1 on failure. */
static int decode_path_only_payload(uint8_t const *payload, uint32_t len, char *out_posix, size_t out_posix_size) {
    if (len < 2) {
        send_status(ST_ERR_BAD_FRAME);
        return -1;
    }
    uint16_t path_len = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    if (path_len == 0 || path_len >= MAX_PATH_BYTES) {
        send_status(ST_ERR_BAD_PATH);
        esp_rom_printf("[deploy] bad path_len %u\n", (unsigned)path_len);
        return -1;
    }
    if ((uint32_t)path_len + 2 > len) {
        send_status(ST_ERR_BAD_FRAME);
        return -1;
    }

    char vms_path[MAX_PATH_BYTES];
    memcpy(vms_path, payload + 2, path_len);
    vms_path[path_len] = 0;

    if (vms_to_posix(vms_path, out_posix, out_posix_size) != 0) {
        send_status(ST_ERR_BAD_PATH);
        esp_rom_printf("[deploy] bad VMS path '%s'\n", vms_path);
        return -1;
    }
    return 0;
}

static void handle_get(uint8_t const *payload, uint32_t len) {
    char posix_path[MAX_PATH_BYTES + 32];
    if (decode_path_only_payload(payload, len, posix_path, sizeof(posix_path)) != 0)
        return;

    esp_rom_printf("[deploy] GET '%s'\n", posix_path);

    int fd = open(posix_path, O_RDONLY);
    if (fd < 0) {
        send_status(ST_ERR_FOPEN);
        esp_rom_printf("[deploy] GET open failed for '%s' (errno=%d)\n", posix_path, errno);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0 || (uint32_t)st.st_size > MAX_PAYLOAD_BYTES) {
        close(fd);
        send_status(ST_ERR_TOO_BIG);
        esp_rom_printf("[deploy] GET '%s' too big or stat failed\n", posix_path);
        return;
    }

    uint32_t file_len = (uint32_t)st.st_size;
    uint8_t *buf      = file_len ? malloc(file_len) : NULL;
    if (file_len && !buf) {
        close(fd);
        send_status(ST_ERR_OOM);
        esp_rom_printf("[deploy] GET OOM for %u bytes\n", (unsigned)file_len);
        return;
    }

    uint32_t total = 0;
    while (total < file_len) {
        ssize_t n = read(fd, buf + total, file_len - total);
        if (n <= 0) {
            close(fd);
            free(buf);
            send_status(ST_ERR_READ);
            esp_rom_printf(
                "[deploy] GET read failed at %u/%u (errno=%d)\n",
                (unsigned)total,
                (unsigned)file_len,
                errno
            );
            return;
        }
        total += (uint32_t)n;
    }
    close(fd);

    esp_rom_printf("[deploy] GET '%s' -> %u bytes OK\n", posix_path, (unsigned)file_len);
    send_response(ST_OK, buf, file_len);
    free(buf);
}

static void handle_list(uint8_t const *payload, uint32_t len) {
    char posix_path[MAX_PATH_BYTES + 32];
    if (decode_path_only_payload(payload, len, posix_path, sizeof(posix_path)) != 0)
        return;

    esp_rom_printf("[deploy] LIST '%s'\n", posix_path);

    DIR *d = opendir(posix_path);
    if (!d) {
        send_status(ST_ERR_FOPEN);
        esp_rom_printf("[deploy] LIST opendir failed for '%s' (errno=%d)\n", posix_path, errno);
        return;
    }

    uint8_t *buf = malloc(LIST_BUFFER_BYTES);
    if (!buf) {
        closedir(d);
        send_status(ST_ERR_OOM);
        return;
    }

    uint32_t       used = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char full[MAX_PATH_BYTES + 32 + sizeof(ent->d_name) + 2];
        snprintf(full, sizeof(full), "%s/%s", posix_path, ent->d_name);
        struct stat st;
        long        size   = 0;
        bool        is_dir = false;
        if (stat(full, &st) == 0) {
            size   = (long)st.st_size;
            is_dir = S_ISDIR(st.st_mode);
        }

        char line[sizeof(ent->d_name) + 80];
        int  n = snprintf(line, sizeof(line), "%s\t%ld\t%c\n", ent->d_name, size, is_dir ? 'D' : 'F');
        if (n <= 0)
            continue;
        if (used + (uint32_t)n > LIST_BUFFER_BYTES)
            break; /* truncate silently rather than overflow; list a subdir to page further */
        memcpy(buf + used, line, (size_t)n);
        used += (uint32_t)n;
    }
    closedir(d);

    esp_rom_printf("[deploy] LIST '%s' -> %u bytes OK\n", posix_path, (unsigned)used);
    send_response(ST_OK, buf, used);
    free(buf);
}

/* Recursively remove a file or directory tree at a POSIX path. Plain
 * opendir/readdir/unlink/rmdir like the rest of this file (see the header
 * comment on why_fopen/why_open are off-limits from a kernel task) - not
 * the app-side rm_rf() in pathfuncs.c, which goes through why_* wrappers. */
static bool delete_recursive(char const *posix_path) {
    struct stat st;
    if (stat(posix_path, &st) != 0)
        return errno == ENOENT; /* already gone counts as success */

    if (!S_ISDIR(st.st_mode))
        return unlink(posix_path) == 0;

    DIR *d = opendir(posix_path);
    if (!d)
        return false;

    bool           ok = true;
    struct dirent *ent;
    while (ok && (ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char child[MAX_PATH_BYTES + 32 + sizeof(ent->d_name) + 2];
        snprintf(child, sizeof(child), "%s/%s", posix_path, ent->d_name);
        ok = delete_recursive(child);
    }
    closedir(d);
    return ok && rmdir(posix_path) == 0;
}

static void handle_delete(uint8_t const *payload, uint32_t len) {
    char posix_path[MAX_PATH_BYTES + 32];
    if (decode_path_only_payload(payload, len, posix_path, sizeof(posix_path)) != 0)
        return;

    esp_rom_printf("[deploy] DELETE '%s'\n", posix_path);

    if (!delete_recursive(posix_path)) {
        send_status(ST_ERR_WRITE);
        esp_rom_printf("[deploy] DELETE failed for '%s' (errno=%d)\n", posix_path, errno);
        return;
    }

    esp_rom_printf("[deploy] DELETE '%s' OK\n", posix_path);
    send_response(ST_OK, NULL, 0);
}

/* ============== Frame reader / dispatcher ============== */

static void process_one_frame(void) {
    /* Header: cmd(1) + len(4) */
    uint8_t hdr[5];
    rx_blocking(hdr, 5);

    uint8_t  cmd = hdr[0];
    uint32_t len = (uint32_t)hdr[1] | ((uint32_t)hdr[2] << 8) | ((uint32_t)hdr[3] << 16) | ((uint32_t)hdr[4] << 24);

    if (len > MAX_PAYLOAD_BYTES) {
        /* Drain CRC bytes (we already have nothing to read for payload) so
         * we stay frame-aligned. We can't safely skip payload bytes without
         * reading them, so just bail and let scan re-sync. */
        send_status(ST_ERR_TOO_BIG);
        esp_rom_printf("[deploy] frame too big: %u\n", (unsigned)len);
        return;
    }

    /* CRC over [cmd:1][len:4][payload:N] -- header folded in now regardless
     * of command; CMD_PUT folds the rest in itself as it streams (see
     * handle_put_streaming()'s own comment), everything else still gets it
     * folded below over one buffered payload. */
    uint16_t crc = 0xFFFF;
    crc          = esp_rom_crc16_le(crc, hdr, 5);

    if (cmd == CMD_PUT) {
        /* Bypasses the malloc(len)-sized-to-the-whole-frame path below --
         * that's what used to OOM on a large file (task #44). */
        handle_put_streaming(len, crc);
        return;
    }

    uint8_t *payload = NULL;
    if (len > 0) {
        payload = malloc(len);
        if (!payload) {
            send_status(ST_ERR_OOM);
            esp_rom_printf("[deploy] OOM for %u-byte payload\n", (unsigned)len);
            return;
        }
        rx_blocking(payload, len);
        crc = esp_rom_crc16_le(crc, payload, len);
    }

    uint8_t crc_bytes[2];
    rx_blocking(crc_bytes, 2);
    uint16_t crc_wire = (uint16_t)crc_bytes[0] | ((uint16_t)crc_bytes[1] << 8);

    if (crc != crc_wire) {
        esp_rom_printf(
            "[deploy] CRC mismatch: wire=0x%04X calc=0x%04X cmd=0x%02X len=%u\n",
            crc_wire,
            crc,
            cmd,
            (unsigned)len
        );
        send_status(ST_ERR_BAD_FRAME);
        free(payload);
        return;
    }

    switch (cmd) {
        case CMD_PING: handle_ping(payload, len); break;
        case CMD_GET: handle_get(payload, len); break;
        case CMD_LIST: handle_list(payload, len); break;
        case CMD_DELETE: handle_delete(payload, len); break;
        default:
            esp_rom_printf("[deploy] unknown cmd 0x%02X\n", cmd);
            send_status(ST_ERR_UNKNOWN_CMD);
            break;
    }

    free(payload);
}

static void deploy_listener_task(void *arg) {
    esp_rom_printf("[deploy] listener active, waiting for magic\n");
    while (1) {
        scan_for_magic();
        process_one_frame();
    }
}

bool deploy_protocol_init(void) {
    esp_rom_printf("[deploy] init: creating listener task\n");
    /* Priority must stay below the wifi hermes task (5, also core 0) - at 6
     * this task permanently starved hermes and everything else <=6 on core 0
     * (confirmed root cause of the wifi-analyzer hang investigation: hermes
     * stuck eReady forever, core-0-pinned diagnostic tasks froze right after
     * this task's creation, and skipping this call entirely kept core 0
     * alive). rx_blocking()'s 2ms poll cadence has no real-time requirement
     * that needs a high priority. */
    BaseType_t r = create_kernel_task(
        deploy_listener_task,
        "deploy",
        6144, /* stack — bigger because we now do fwrite/malloc */
        NULL,
        3,
        &deploy_handle,
        0
    );
    esp_rom_printf("[deploy] init: create_kernel_task returned %d\n", (int)r);
    if (r != pdTRUE) {
        esp_rom_printf("[deploy] init: FAILED to create task\n");
        return false;
    }
    return true;
}
