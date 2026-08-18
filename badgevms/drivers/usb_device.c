/* This file is part of BadgeVMS
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* BadgeLink transport binding: native USB (P4 HS-OTG PHY via the WHY2025
 * carrier's GPIO2 mux), superseding the earlier UART0 attempt.
 *
 * 2026-08-07's badgelink_transport_uart.c (removed 2026-08-16, see the
 * design doc below) concluded native USB was physically impossible on this
 * carrier: the P4's native-USB pins looked unrouted on the public schematics,
 * and the bottom USB-C port appeared wired exclusively to the C6.
 *
 * That conclusion was incomplete. 2026-08-16, re-testing the bottom port
 * with Senna-chan's tanmatsu-launcher WHY2025 port (rather than this
 * firmware) showed it enumerate as BadgeLink's own vendor device
 * (16d0:0f9a "MCS WHY2025") and badgelink.py worked immediately. Her
 * usb_device.c (senna_idf6_native/main/usb_device.c) shows why: GPIO2 is a
 * mux-select pin. High (the default) routes the connector's D+/D- to the
 * C6's own native USB; low routes them to the P4's own High-Speed OTG PHY
 * instead. The PHY bring-up below (usb_new_phy() + TINYUSB_PORT_HIGH_SPEED_0
 * + the GPIO2 toggle) is ported directly from her code, which is the only
 * hardware-confirmed reference for this sequence that exists right now.
 *
 * Hardware-boot-verified 2026-08-16: flashed to a real badge with the mux
 * forced to the P4 at boot, booted clean (no crash/panic around the
 * TinyUSB/mux init). Not yet independently confirmed from a second machine
 * that the bottom port actually enumerates as 16d0:0f9a -- see
 * docs/design/badgelink-usb-port.md.
 *
 * Also unresolved: whether GPIO2 collides with the TCA8418 keyboard
 * interrupt line badge-bsp calls BSP_KBD_INT (same pin number, see
 * docs/tanmatsu-launcher-port-analysis.md); BadgeVMS's own TCA8418 driver
 * polls rather than using an interrupt (tca8418.c), so nothing in this
 * codebase currently drives GPIO2 as an input -- but that's a "no known
 * conflict today", not a hardware-verified "safe by design".
 *
 * Unlike the UART transport, this one shares no bus with deploy_protocol.c
 * (which stays on UART0/CH340), so both can run at the same time -- no
 * mutual-exclusion gate is needed between them.
 *
 * The mux personality is app-controlled (usb_device_switch_to(), reached via
 * badgevms/usb_device_bridge.c's bv_usb_device_set_mode() -- see
 * cj_launcher's diamond-key handling in the separate why2025-apps repo),
 * not forced at boot: TinyUSB comes up here, but the mux stays on its C6
 * default until something actually asks for BadgeLink or MSC mode. */

#include "usb_device.h"

#include "badgelink.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "usb_msc.h"

#include <stdio.h>

#include <driver/gpio.h>

static char const *TAG = "usb_device";

static usb_device_mode_t current_mode = USB_DEVICE_MODE_DEBUG;

/* GPIO2 on the P4: WHY2025 carrier's USB mux select. Low = P4 HS-OTG PHY
 * (BadgeLink device mode), high = C6 native USB (default/debug). Same pin
 * number as badge-bsp's BSP_KBD_INT -- see the file header. */
#define WHY2025_USB_MUX_GPIO 2

// MSC's own interface -- alongside BadgeLink's vendor interface in the same
// composite device, not a separate USB identity. All the tud_msc_*
// callbacks TinyUSB calls into for this interface are implemented by
// espressif/esp_tinyusb's tinyusb_msc.c, not here -- see usb_msc.c, which
// only calls that component's high-level storage-instance/mount-point API.
enum usb_device_interface { ITF_NUM_VENDOR = 0, ITF_NUM_MSC, ITF_COUNT };
enum usb_device_endpoint { EP_EMPTY = 0, EPNUM_VENDOR, EPNUM_MSC };

#define USB_STRING_LENGTH 32
static char usb_vendor[USB_STRING_LENGTH]  = "DutchVMS";
static char usb_product[USB_STRING_LENGTH] = "WHY2025 badge";
static char usb_serial[USB_STRING_LENGTH];

static char const *s_str_desc[6] = {
    (char[]){0x09, 0x04}, // 0: English (0x0409)
    usb_vendor,           // 1: Manufacturer
    usb_product,          // 2: Product
    "BadgeLink",          // 3: Control interface
    usb_serial,           // 4: Serial, chip ID
    "Mass Storage",       // 5: MSC interface
};

enum {
    STRING_DESC = 0,
    STRING_DESC_MANUFACTURER,
    STRING_DESC_PRODUCT,
    STRING_DESC_VENDOR,
    STRING_DESC_SERIAL,
    STRING_DESC_MSC,
};

/* Same VID:PID as every other badge.team BadgeLink device (MCH2022's
 * original allocation, reused as the protocol's own identifier) --
 * badgelink.py auto-discovers by this exact pair, not by product string. */
static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0210,
    .bDeviceClass       = 0,
    .bDeviceSubClass    = 0,
    .bDeviceProtocol    = 0,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x16D0,
    .idProduct          = 0x0F9A,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRING_DESC_MANUFACTURER,
    .iProduct           = STRING_DESC_PRODUCT,
    .iSerialNumber      = STRING_DESC_SERIAL,
    .bNumConfigurations = 0x01,
};

#define USB_DEVICE_DESC_TOTAL_LEN                                                                                      \
    (TUD_CONFIG_DESC_LEN + CFG_TUD_VENDOR * TUD_VENDOR_DESC_LEN + CFG_TUD_MSC * TUD_MSC_DESC_LEN)

// Full-speed fallback config (required by the USB spec even though this
// port is enumerated through the P4's high-speed OTG PHY) -- endpoint size
// capped at 64 bytes, matching full-speed's own limit.
static uint8_t const s_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, USB_DEVICE_DESC_TOTAL_LEN, 0, 100),
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, STRING_DESC_VENDOR, EPNUM_VENDOR, (0x80 | EPNUM_VENDOR), 32),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRING_DESC_MSC, EPNUM_MSC, (0x80 | EPNUM_MSC), 64),
};
static uint8_t const s_cfg_desc_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, USB_DEVICE_DESC_TOTAL_LEN, 0, 100),
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, STRING_DESC_VENDOR, EPNUM_VENDOR, (0x80 | EPNUM_VENDOR), 512),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRING_DESC_MSC, EPNUM_MSC, (0x80 | EPNUM_MSC), 512),
};

/* BadgeLink's usb_callback_t: send raw bytes out over the vendor endpoint. */
static void usb_device_send_data(uint8_t const *data, size_t len) {
    while (len) {
        uint32_t sent = tud_vendor_write(data, len);
        tud_vendor_write_flush();
        data += sent;
        len  -= sent;
    }
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    (void)rhport;
    (void)request;
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    // No WebUSB/vendor control requests implemented -- BadgeLink itself
    // runs entirely over the vendor bulk endpoints, not control transfers.
    return false;
}

void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize) {
    if (itf != 0) {
        ESP_LOGE(TAG, "tud_vendor_rx_cb on unexpected interface %u", itf);
        return;
    }
    if (bufsize > 0) {
        badgelink_rxdata_cb(buffer, bufsize);
#if CFG_TUD_VENDOR_RX_BUFSIZE > 0
        tud_vendor_read_flush();
#endif
    } else {
        uint8_t  rx_buf[64];
        uint32_t available;
        while ((available = tud_vendor_available()) > 0) {
            uint32_t read = tud_vendor_read(rx_buf, sizeof(rx_buf));
            badgelink_rxdata_cb(rx_buf, read);
        }
    }
}

/* Flip the physical GPIO2 mux + TinyUSB connect state. Only handles the
 * mux/bus side -- doesn't know about MSC or badgelink specifically, both
 * usb_device_switch_to() and the badgelink-callback wrapper below call
 * this. */
static void usb_device_mux_set(bool to_p4) {
    tud_disconnect();
    gpio_set_level(WHY2025_USB_MUX_GPIO, to_p4 ? 0 : 1);
    if (to_p4) {
        vTaskDelay(pdMS_TO_TICKS(500));
        tud_connect();
        ESP_LOGI(TAG, "USB mux -> P4");
    } else {
        ESP_LOGI(TAG, "USB mux -> C6 (debug/default)");
    }
}

/* usb_msc_activate()/deactivate() unmount/remount FLASH0's FAT partition via
 * esp_tinyusb, which frees structures that usb_msc_init() originally
 * allocated at boot, in kernel task context. BadgeVMS gives every app task
 * its own private dlmalloc arena (badgevms/memory_get_malloc_info.h's
 * get_malloc_state() resolves dlmalloc's "global" state per calling task),
 * so calling this straight from an app task -- as this function used to --
 * frees kernel-arena memory from the app's arena. dlmalloc's own
 * USAGE_ERROR_ACTION catches the arena mismatch and aborts (that's the
 * "Task N caused an unhandled exception" / dlfree() abort seen activating
 * MSC from cj_usb_msc, root-caused 2026-08-16). Fix: do the actual mode
 * switch on a dedicated kernel task (same untagged-task trick as
 * Zeus/Hades, see create_kernel_task()) so it always runs in the kernel
 * arena, matching where usb_msc_init() allocated everything. The calling
 * app task just posts a request and blocks on its own semaphore. */
typedef struct {
    usb_device_mode_t mode;
    SemaphoreHandle_t done;
    bool              result;
} usb_device_switch_request_t;

static QueueHandle_t usb_device_switch_queue;

static bool usb_device_switch_to_impl(usb_device_mode_t mode) {
    if (mode == current_mode) {
        return true;
    }

    // Leaving MSC (or entering it): usb_msc_activate()/deactivate() hand
    // FLASH0's (and SD0's, if present) FAT mount back and forth between
    // BadgeVMS's own kernel VFS and the USB host -- see usb_msc.c.
    if (current_mode == USB_DEVICE_MODE_MSC) {
        if (!usb_msc_deactivate()) {
            ESP_LOGE(TAG, "usb_msc_deactivate failed, refusing to switch away from MSC");
            return false;
        }
    }

    switch (mode) {
        case USB_DEVICE_MODE_DEBUG: usb_device_mux_set(false); break;
        case USB_DEVICE_MODE_BADGELINK: usb_device_mux_set(true); break;
        case USB_DEVICE_MODE_MSC:
            if (!usb_msc_activate()) {
                ESP_LOGE(TAG, "usb_msc_activate failed, not switching to MSC mode");
                return false;
            }
            usb_device_mux_set(true);
            break;
        default: return false;
    }

    current_mode = mode;
    return true;
}

static void usb_device_worker_task(void *arg) {
    (void)arg;
    usb_device_switch_request_t *req;
    for (;;) {
        if (xQueueReceive(usb_device_switch_queue, &req, portMAX_DELAY) == pdTRUE) {
            req->result = usb_device_switch_to_impl(req->mode);
            xSemaphoreGive(req->done);
        }
    }
}

bool usb_device_switch_to(usb_device_mode_t mode) {
    usb_device_switch_request_t  req     = {.mode = mode, .done = xSemaphoreCreateBinary()};
    usb_device_switch_request_t *req_ptr = &req;
    if (!req.done) {
        ESP_LOGE(TAG, "Failed to allocate semaphore for USB mode switch");
        return false;
    }
    xQueueSend(usb_device_switch_queue, &req_ptr, portMAX_DELAY);
    xSemaphoreTake(req.done, portMAX_DELAY);
    vSemaphoreDelete(req.done);
    return req.result;
}

usb_device_mode_t usb_device_get_current_mode(void) {
    return current_mode;
}

/* badgelink's own SetUsbMode request only knows DEBUG/DEVICE -- map that
 * onto our DEBUG/BADGELINK. Registered as badgelink's set-usb-mode
 * callback, so a connected BadgeLink client can also request the switch
 * back to debug mode via badgelink.py's SetUsbMode request. */
static void usb_device_badgelink_mode_cb(badgelink_usb_mode_t mode) {
    switch (mode) {
        case BADGELINK_USB_MODE_DEVICE: usb_device_switch_to(USB_DEVICE_MODE_BADGELINK); break;
        case BADGELINK_USB_MODE_DEBUG: usb_device_switch_to(USB_DEVICE_MODE_DEBUG); break;
    }
}

bool usb_device_init(void) {
    // No bsp_device_get_manufacturer()/get_name() on BadgeVMS (that's a
    // badge-bsp API) -- usb_vendor/usb_product above are static instead.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(
        usb_serial,
        USB_STRING_LENGTH,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5]
    );

    /* gpio_reset_pin() first: this codebase already hit the exact same class
     * of bug on GPIO3 (board_bringup.c's vibrator-motor fix) -- a plain
     * gpio_set_direction()/gpio_config() alone didn't reliably take hold of
     * the pad, the pin needed to be explicitly detached from its default
     * IOMUX function first. GPIO2 has the same profile here: it's not a
     * plain unused GPIO, badge-bsp's own hardware.h calls it BSP_KBD_INT, so
     * whatever IOMUX/pull state it resets to at boot may not already be
     * "generic output" the way an actually-unused pin's would be. Senna's
     * reference code (which does enumerate on real hardware) never called
     * this either, but her badge-bsp does its own board-wide pin bring-up
     * before user code like usb_device.c runs -- BadgeVMS has no equivalent
     * step, so this firmware can't assume GPIO2 starts in the same state
     * hers does. */
    gpio_reset_pin(WHY2025_USB_MUX_GPIO);
    gpio_set_direction(WHY2025_USB_MUX_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(WHY2025_USB_MUX_GPIO, 1); // default: mux -> C6, matches Senna's reference default.

    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,
        .otg_mode   = USB_OTG_MODE_DEVICE,
    };
    usb_phy_handle_t phy_handle = NULL;
    esp_err_t        phy_err    = usb_new_phy(&phy_conf, &phy_handle);
    if (phy_err != ESP_OK) {
        ESP_LOGE(TAG, "usb_new_phy failed: %s", esp_err_to_name(phy_err));
        return false;
    }

    tinyusb_config_t tusb_cfg             = TINYUSB_CONFIG_FULL_SPEED(NULL, NULL);
    tusb_cfg.phy.skip_setup               = true;
    tusb_cfg.port                         = TINYUSB_PORT_HIGH_SPEED_0;
    tusb_cfg.descriptor.device            = &desc_device;
    tusb_cfg.descriptor.string            = s_str_desc;
    tusb_cfg.descriptor.string_count      = sizeof(s_str_desc) / sizeof(s_str_desc[0]);
    tusb_cfg.descriptor.full_speed_config = s_cfg_desc;
    tusb_cfg.descriptor.high_speed_config = s_cfg_desc_hs;

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "TinyUSB device stack up (mux defaulting to C6)");

    badgelink_init();
    badgelink_set_usb_mode_callback(usb_device_badgelink_mode_cb);
    badgelink_start(usb_device_send_data);

    usb_device_switch_queue = xQueueCreate(1, sizeof(usb_device_switch_request_t *));
    if (!usb_device_switch_queue) {
        ESP_LOGE(TAG, "Failed to create USB mode-switch queue");
        return false;
    }
    TaskHandle_t worker_handle;
    if (create_kernel_task(usb_device_worker_task, "usb_device", 4096, NULL, 6, &worker_handle, 1) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create USB mode-switch worker task");
        return false;
    }

    return true;
}
