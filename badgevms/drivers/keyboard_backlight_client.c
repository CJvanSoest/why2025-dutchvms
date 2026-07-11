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

/* See badgevms/keyboard_backlight.h. Sends a one-byte percent value to the
 * C6's keyboard-backlight PWM channel over the esp-hosted custom_data
 * channel -- same transport lora_proto_client.c uses for the (much more
 * involved) LoRa request/reply protocol, but this is fire-and-forget: no
 * response is expected, so there's no callback registration or reply
 * wait here. */

#include "badgevms/keyboard_backlight.h"
#include "esp_hosted.h"
#include "esp_log.h"

static char const *TAG = "kb_backlight_client";

/* Must match connectivity_esp_hosted/slave/main/tanmatsu/priv_events.h --
 * mirrored locally rather than shared via include path, same convention
 * lora_proto_client.c's own TANMATSU_EVENT_LORA define uses (P4 kernel and
 * C6 slave firmware are separate build targets in this monorepo). */
#define TANMATSU_EVENT_KEYBOARD_BL 0x05

void bv_keyboard_set_backlight(uint8_t percent) {
    if (percent > 100)
        percent = 100;
    esp_err_t res = esp_hosted_send_custom_data(TANMATSU_EVENT_KEYBOARD_BL, &percent, 1);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send keyboard backlight update: %s", esp_err_to_name(res));
    }
}
