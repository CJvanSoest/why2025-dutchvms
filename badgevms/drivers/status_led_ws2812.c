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

/* 4x RGBW status LEDs (SK6812/XL-5050RGBWC-style) on WS-DATA = GPIO7, driven
 * via the RMT peripheral. Split out of badgevms_i2c_bus.c, which now only
 * holds the generic I2C bus VFS layer -- see drivers/board_bringup.c for
 * when this task is started and drivers/status_led_internal.h for the
 * app-facing control surface this exposes (status_led_set/show/clear/
 * set_brightness, bv_led_app_control).
 *
 * Status indicator legend:
 *   LED0 = LoRa radio  : green = up/active, blue = starting up, red = offline
 *   LED1 = WiFi        : green = connected, blue = enabled/connecting, off = disabled
 *   LED2       = LED-notify (DM)      : notify.c identifier "cj_meshcore.dm"
 *   LED3       = LED-notify (channel) : notify.c identifier "cj_meshcore.channel"
 *                Each LED is driven independently off its own notify_get_dirty()
 *                identifier rather than the shared notify_any_dirty() aggregate,
 *                so LED2 lights only for unread DMs and LED3 only for unread
 *                channel messages (e.g. LED2 blinking with LED3 off means "unread
 *                DMs, no unread channel traffic"). This deliberately couples the
 *                kernel LED driver to cj_meshcore's specific notify identifiers
 *                instead of staying app-agnostic like notify_any_dirty() - not
 *                generic, but acceptable on this personal single-user badge
 *                where cj_meshcore is the only app that uses LED-notify today.
 *                Slow ~0.5 Hz on/off pulse, LED2=yellow (DM) / LED3=blue
 *                (channel) via explicit R/G/B (CJ's chosen colors) - NOT the
 *                W channel: the WHY2025_demo.ino reference sketch for this
 *                same XL-5050RGBWC hardware never uses W for anything (always
 *                passes 0), and testing confirmed the W-channel die on this
 *                part reads as blue-tinted rather than neutral white, so W is
 *                unreliable for a specific expected color here. Still 4-byte
 *                G,R,B,W protocol via ws2812_set()/ws2812_show() below (this
 *                add-on's LEDs are XL-5050RGBWC, NOT plain WS2812B); W is just
 *                always 0 for these two.
 * State comes from the firmware-internal LoRa/WiFi query APIs plus notify.c. */

#include "status_led_ws2812.h"

#include "badgevms/lora.h"
#include "badgevms/notify.h"
#include "badgevms/wifi.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status_led_internal.h"

#include <stdbool.h>

#define TAG "status_led_ws2812"

#define WS_GPIO  7
#define WS_COUNT 4
static rmt_channel_handle_t ws_chan = NULL;
static rmt_encoder_handle_t ws_enc  = NULL;
static uint8_t              ws_grbw[WS_COUNT * 4]; /* per LED: G, R, B, W */

static bool ws2812_init(void) {
    rmt_tx_channel_config_t txc = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .gpio_num          = WS_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz     = 10000000, /* 0.1us / tick */
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&txc, &ws_chan) != ESP_OK)
        return false;
    rmt_bytes_encoder_config_t bc = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9}, /* 0.3us / 0.9us */
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 5}, /* 0.9us / 0.5us (closer to typ 0.45us) */
        .flags.msb_first = 1,
    };
    if (rmt_new_bytes_encoder(&bc, &ws_enc) != ESP_OK)
        return false;
    return rmt_enable(ws_chan) == ESP_OK;
}
static void ws2812_set(int i, uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    if ((unsigned)i >= WS_COUNT)
        return;
    ws_grbw[i * 4 + 0] = g;
    ws_grbw[i * 4 + 1] = r;
    ws_grbw[i * 4 + 2] = b;
    ws_grbw[i * 4 + 3] = w;
}
static void ws2812_show(void) {
    rmt_transmit_config_t tc = {.loop_count = 0};
    rmt_transmit(ws_chan, ws_enc, ws_grbw, sizeof(ws_grbw), &tc);
    rmt_tx_wait_all_done(ws_chan, 100);
}
/* Scale an RGB triple to ~LED_BRIGHTNESS% and push to one LED (W=0). */
#define LED_BRIGHTNESS 15
static void ws2812_set_scaled(int i, uint8_t r, uint8_t g, uint8_t b) {
    ws2812_set(i, (r * LED_BRIGHTNESS) / 100, (g * LED_BRIGHTNESS) / 100, (b * LED_BRIGHTNESS) / 100, 0);
}

/* ---- App-facing control of the 4 RGBW status LEDs (see
 * badgevms/status_led.h + status_led_bridge.c, exposed here non-static via
 * drivers/status_led_internal.h - same pattern as led_matrix_clear/pixel/
 * row/fill/brightness in led_matrix_pca9698.c and bv_mtx_app_control's
 * take()/release() arbitration for the matrix). bv_led_app_control lets an
 * app take exclusive ownership of the shared ws_grbw chain: while true,
 * ws2812_task below skips its own radio/wifi/notify computation AND its own
 * ws2812_show() entirely (not just the color computation - otherwise it
 * would still stomp the app's frame every ~1s by calling show() with a
 * buffer the app didn't write), so the app's last-pushed frame stays exactly
 * as drawn. On release, the task's next ~1s tick recomputes real status and
 * redraws it, same as the matrix's mtx_demo_task resuming once
 * bv_mtx_app_control drops. */
bool volatile bv_led_app_control = false;

/* Global brightness for app-driven status-LED writes, independent of the
 * firmware's own fixed LED_BRIGHTNESS - mirrors led_matrix_brightness()'s
 * "persists across take()/release(), not reset on release" precedent. */
static int status_led_app_brightness = LED_BRIGHTNESS;

void status_led_set(int i, uint8_t r, uint8_t g, uint8_t b) {
    ws2812_set(
        i,
        (uint8_t)((r * status_led_app_brightness) / 100),
        (uint8_t)((g * status_led_app_brightness) / 100),
        (uint8_t)((b * status_led_app_brightness) / 100),
        0
    );
}

void status_led_show(void) {
    if (!ws_chan) /* ws2812_task hasn't finished RMT init yet - nothing to push to */
        return;
    ws2812_show();
}

void status_led_clear(void) {
    for (int i = 0; i < WS_COUNT; i++) ws2812_set(i, 0, 0, 0, 0);
}

void status_led_set_brightness(int pct) {
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    status_led_app_brightness = pct;
}

#define LORA_STARTUP_GRACE_S 15
static void ws2812_task(void *arg) {
    (void)arg;
    if (!ws2812_init()) {
        ESP_LOGW(TAG, "WS2812 init failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "=== RGBW status LEDs (4x on GPIO%d) START: LED0=radio LED1=wifi LED2/3=notify ===", WS_GPIO);
    uint32_t secs = 0;
    for (;;) {
        if (bv_led_app_control) {
            /* An app has taken control of the shared ws_grbw chain (see
             * status_led_set()/status_led_show() above) - don't touch it,
             * don't even call ws2812_show(), just idle (still yielding so
             * idle/WDT are fed), same as mtx_demo_task's bv_mtx_app_control
             * handling for the matrix. */
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        /* LED0: LoRa radio status */
        lora_mode_t   mode;
        lora_status_t lst;
        if (lora_get_mode(&mode) && mode != LORA_MODE_UNKNOWN) {
            ws2812_set_scaled(0, 0, 255, 0); /* green: up/active */
        } else if (lora_get_status(&lst) && lst.chip_type != LORA_CHIP_UNKNOWN) {
            ws2812_set_scaled(0, 0, 255, 0); /* green: radio responding */
        } else if (secs < LORA_STARTUP_GRACE_S) {
            ws2812_set_scaled(0, 0, 0, 255); /* blue: still starting up */
        } else {
            ws2812_set_scaled(0, 255, 0, 0); /* red: offline / no response */
        }

        /* LED1: WiFi status (Bluetooth has no status API yet) */
        if (wifi_get_status() == WIFI_DISABLED) {
            ws2812_set_scaled(1, 0, 0, 0); /* off: disabled */
        } else if (wifi_get_connection_status() == WIFI_CONNECTED) {
            ws2812_set_scaled(1, 0, 255, 0); /* green: connected */
        } else {
            ws2812_set_scaled(1, 0, 0, 255); /* blue: enabled / connecting */
        }

        /* LED2, LED3: LED-notify, each on its own cj_meshcore notify.c
         * identifier (see file header comment above) instead of the shared
         * notify_any_dirty() aggregate - LED2=DM (yellow), LED3=channel
         * (blue), independently on/off. Cheap poll (once per this task's
         * existing 1s cadence - no separate task/timer needed). Blinking
         * every other second (~0.5 Hz) so it reads as "waiting for
         * attention" rather than a steady-on light. */
        bool const dm_on = (secs % 2 == 0) && notify_get_dirty("cj_meshcore.dm");
        bool const ch_on = (secs % 2 == 0) && notify_get_dirty("cj_meshcore.channel");
        if (dm_on) {
            ws2812_set_scaled(2, 255, 255, 0); /* yellow: unread DM */
        } else {
            ws2812_set_scaled(2, 0, 0, 0);
        }
        if (ch_on) {
            ws2812_set_scaled(3, 0, 0, 255); /* blue: unread channel message */
        } else {
            ws2812_set_scaled(3, 0, 0, 0);
        }

        ws2812_show();
        vTaskDelay(pdMS_TO_TICKS(1000));
        secs += 1;
    }
}

void status_led_ws2812_start(void) {
    xTaskCreate(ws2812_task, "ws2812", 4096, NULL, 2, NULL);
}
