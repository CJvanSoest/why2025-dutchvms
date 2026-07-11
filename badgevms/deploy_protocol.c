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

/* PUT reads+CRCs+writes its payload in bounded chunks straight off the
 * wire instead of malloc()ing the whole thing up front (the rest of this
 * file's PING/GET/LIST/DELETE handlers still go through
 * process_one_frame()'s generic malloc-the-whole-payload path below --
 * their payloads are either a bare path (<=MAX_PATH_BYTES) or, for GET,
 * bounded by SD-card file sizes nobody has hit a wall on yet). This is
 * what actually removes the "app binary big enough hits ERR_OOM" ceiling
 * (why2025-apps#1 hardware-test feedback, hit at ~155KB well under this
 * project's own documented "~1MB, and that's an accepted limit" -- see
 * .claude/Pitfalls.md's "UART deploy protocol has a request-size ceiling"
 * entry, now stale for PUT specifically): RAM use is bounded by
 * PUT_CHUNK_BYTES regardless of file size, not by the whole file fitting
 * in one contiguous heap block (which, worse, competes with every other
 * subsystem's allocations after a long uptime -- see that same feedback
 * thread for why a 2MB MAX_PAYLOAD_BYTES cap didn't save us: heap
 * fragmentation can fail a malloc() far below the cap).
 *
 * Frame layout is unchanged (see the file header comment) -- this handler
 * is just a different way of *reading* the same bytes. hdr is the already-
 * read [cmd:1][len:4] header (still hashed into the CRC, same as the
 * generic path). Malformed-frame error paths deliberately don't try to
 * drain the rest of the frame before returning -- same tolerance
 * process_one_frame()'s own len>MAX_PAYLOAD_BYTES case already has;
 * deploy_listener_task()'s scan_for_magic() re-syncs on the next frame
 * regardless. */
#define PUT_CHUNK_BYTES 4096

static void handle_put_streamed(uint8_t const hdr[5], uint32_t len) {
    uint16_t crc = 0xFFFF;
    crc          = esp_rom_crc16_le(crc, hdr, 5);

    if (len < 2) {
        send_status(ST_ERR_BAD_FRAME);
        esp_rom_printf("[deploy] PUT: len<2\n");
        return;
    }

    uint8_t path_len_bytes[2];
    rx_blocking(path_len_bytes, 2);
    crc                = esp_rom_crc16_le(crc, path_len_bytes, 2);
    uint16_t path_len  = (uint16_t)path_len_bytes[0] | ((uint16_t)path_len_bytes[1] << 8);
    uint32_t remaining = len - 2; /* path + file data, not yet read */

    if (path_len == 0 || path_len >= MAX_PATH_BYTES || (uint32_t)path_len > remaining) {
        send_status(path_len > remaining ? ST_ERR_BAD_FRAME : ST_ERR_BAD_PATH);
        esp_rom_printf("[deploy] PUT: bad path_len %u\n", (unsigned)path_len);
        return;
    }

    char vms_path[MAX_PATH_BYTES];
    rx_blocking((uint8_t *)vms_path, path_len);
    crc                = esp_rom_crc16_le(crc, (uint8_t const *)vms_path, path_len);
    vms_path[path_len] = 0;
    uint32_t data_len  = remaining - path_len;

    char posix_path[MAX_PATH_BYTES + 32];
    if (vms_to_posix(vms_path, posix_path, sizeof(posix_path)) != 0) {
        send_status(ST_ERR_BAD_PATH);
        esp_rom_printf("[deploy] PUT: bad VMS path '%s'\n", vms_path);
        return;
    }

    esp_rom_printf("[deploy] PUT '%s' -> '%s' %u bytes (streamed)\n", vms_path, posix_path, (unsigned)data_len);

    /* mkdir -p: create any missing parent directories. We mutate posix_path
     * temporarily by null-terminating at each '/' and calling mkdir. */
    for (char *p = posix_path + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(posix_path, 0755); /* ignore errors (EEXIST is fine) */
            *p = '/';
        }
    }

    int fd = open(posix_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        send_status(ST_ERR_FOPEN);
        esp_rom_printf("[deploy] PUT open failed for '%s' (errno=%d)\n", posix_path, errno);
        return;
    }

    uint8_t  chunk[PUT_CHUNK_BYTES];
    uint32_t remain   = data_len;
    bool     write_ok = true;
    while (remain > 0) {
        uint32_t n = remain < sizeof(chunk) ? remain : sizeof(chunk);
        rx_blocking(chunk, n);
        crc = esp_rom_crc16_le(crc, chunk, n);
        if (write_ok) {
            size_t written = 0;
            while (written < n) {
                ssize_t w = write(fd, chunk + written, n - written);
                if (w <= 0) {
                    write_ok = false;
                    break;
                }
                written += (size_t)w;
            }
        }
        remain -= n;
    }
    close(fd);

    uint8_t crc_bytes[2];
    rx_blocking(crc_bytes, 2);
    uint16_t crc_wire = (uint16_t)crc_bytes[0] | ((uint16_t)crc_bytes[1] << 8);

    if (crc != crc_wire) {
        unlink(posix_path); /* don't leave a corrupt partial file behind */
        send_status(ST_ERR_BAD_FRAME);
        esp_rom_printf("[deploy] PUT CRC mismatch: wire=0x%04X calc=0x%04X\n", crc_wire, crc);
        return;
    }
    if (!write_ok) {
        send_status(ST_ERR_WRITE);
        esp_rom_printf("[deploy] PUT write failed for '%s' (errno=%d)\n", posix_path, errno);
        return;
    }

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

    /* PUT reads its own payload straight off the wire in bounded chunks
     * instead of through the generic malloc-the-whole-payload path below --
     * see handle_put_streamed()'s own comment for why. Every other command's
     * payload is small (a bare path) or, for GET, still goes through the
     * generic path -- see MAX_PAYLOAD_BYTES's own scope. */
    if (cmd == CMD_PUT) {
        handle_put_streamed(hdr, len);
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
    }

    uint8_t crc_bytes[2];
    rx_blocking(crc_bytes, 2);
    uint16_t crc_wire = (uint16_t)crc_bytes[0] | ((uint16_t)crc_bytes[1] << 8);

    /* CRC over [cmd:1][len:4][payload:N] */
    uint16_t crc = 0xFFFF;
    crc          = esp_rom_crc16_le(crc, hdr, 5);
    if (payload && len)
        crc = esp_rom_crc16_le(crc, payload, len);

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
