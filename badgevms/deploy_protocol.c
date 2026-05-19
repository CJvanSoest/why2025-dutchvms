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

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

#define MAX_PAYLOAD_BYTES (2 * 1024 * 1024)  /* 2 MB cap */
#define MAX_PATH_BYTES    256

#define CMD_PUT   0x01
#define CMD_PING  0x07

#define ST_OK              0x00
#define ST_ERR_BAD_FRAME   0x01
#define ST_ERR_OOM         0x02
#define ST_ERR_BAD_PATH    0x03
#define ST_ERR_FOPEN       0x04
#define ST_ERR_WRITE       0x05
#define ST_ERR_UNKNOWN_CMD 0x06
#define ST_ERR_TOO_BIG     0x07

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

    char const *rest    = colon + 1;
    char const *lb      = strchr(rest, '[');
    char const *rb      = lb ? strchr(lb, ']') : NULL;
    char const *fname   = rest;
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
    return 0;
}

static void handle_put(uint8_t const *payload, uint32_t len) {
    if (len < 2) {
        send_status(ST_ERR_BAD_FRAME);
        esp_rom_printf("[deploy] PUT: len<2\n");
        return;
    }
    uint16_t path_len = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    if (path_len == 0 || path_len >= MAX_PATH_BYTES) {
        send_status(ST_ERR_BAD_PATH);
        esp_rom_printf("[deploy] PUT: bad path_len %u\n", (unsigned)path_len);
        return;
    }
    if ((uint32_t)path_len + 2 > len) {
        send_status(ST_ERR_BAD_FRAME);
        esp_rom_printf("[deploy] PUT: path_len overruns payload\n");
        return;
    }

    char vms_path[MAX_PATH_BYTES];
    memcpy(vms_path, payload + 2, path_len);
    vms_path[path_len] = 0;

    uint8_t const *data     = payload + 2 + path_len;
    uint32_t       data_len = len - 2 - path_len;

    char posix_path[MAX_PATH_BYTES + 32];
    if (vms_to_posix(vms_path, posix_path, sizeof(posix_path)) != 0) {
        send_status(ST_ERR_BAD_PATH);
        esp_rom_printf("[deploy] PUT: bad VMS path '%s'\n", vms_path);
        return;
    }

    esp_rom_printf("[deploy] PUT '%s' -> '%s' %u bytes\n",
                   vms_path, posix_path, (unsigned)data_len);

    /* mkdir -p: create any missing parent directories. We mutate posix_path
     * temporarily by null-terminating at each '/' and calling mkdir. */
    for (char *p = posix_path + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(posix_path, 0755);   /* ignore errors (EEXIST is fine) */
            *p = '/';
        }
    }

    int fd = open(posix_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        send_status(ST_ERR_FOPEN);
        esp_rom_printf("[deploy] PUT open failed for '%s' (errno=%d)\n",
                       posix_path, errno);
        return;
    }

    size_t   total   = 0;
    uint32_t remain  = data_len;
    while (remain > 0) {
        ssize_t n = write(fd, data + total, remain);
        if (n <= 0) {
            close(fd);
            send_status(ST_ERR_WRITE);
            esp_rom_printf("[deploy] PUT write failed at %u/%u (errno=%d)\n",
                           (unsigned)total, (unsigned)data_len, errno);
            return;
        }
        total  += (size_t)n;
        remain -= (uint32_t)n;
    }
    close(fd);

    uint8_t reply[4] = {
        (uint8_t)(data_len & 0xFF),
        (uint8_t)((data_len >> 8) & 0xFF),
        (uint8_t)((data_len >> 16) & 0xFF),
        (uint8_t)((data_len >> 24) & 0xFF),
    };
    send_response(ST_OK, reply, 4);
}

/* ============== Frame reader / dispatcher ============== */

static void process_one_frame(void) {
    /* Header: cmd(1) + len(4) */
    uint8_t hdr[5];
    rx_blocking(hdr, 5);

    uint8_t  cmd = hdr[0];
    uint32_t len = (uint32_t)hdr[1] | ((uint32_t)hdr[2] << 8) |
                   ((uint32_t)hdr[3] << 16) | ((uint32_t)hdr[4] << 24);

    if (len > MAX_PAYLOAD_BYTES) {
        /* Drain CRC bytes (we already have nothing to read for payload) so
         * we stay frame-aligned. We can't safely skip payload bytes without
         * reading them, so just bail and let scan re-sync. */
        send_status(ST_ERR_TOO_BIG);
        esp_rom_printf("[deploy] frame too big: %u\n", (unsigned)len);
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
    uint16_t crc_wire =
        (uint16_t)crc_bytes[0] | ((uint16_t)crc_bytes[1] << 8);

    /* CRC over [cmd:1][len:4][payload:N] */
    uint16_t crc = 0xFFFF;
    crc          = esp_rom_crc16_le(crc, hdr, 5);
    if (payload && len)
        crc = esp_rom_crc16_le(crc, payload, len);

    if (crc != crc_wire) {
        esp_rom_printf("[deploy] CRC mismatch: wire=0x%04X calc=0x%04X cmd=0x%02X len=%u\n",
                       crc_wire, crc, cmd, (unsigned)len);
        send_status(ST_ERR_BAD_FRAME);
        free(payload);
        return;
    }

    switch (cmd) {
    case CMD_PING:
        handle_ping(payload, len);
        break;
    case CMD_PUT:
        handle_put(payload, len);
        break;
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
    BaseType_t r = create_kernel_task(
        deploy_listener_task,
        "deploy",
        6144,  /* stack — bigger because we now do fwrite/malloc */
        NULL,
        6,
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
