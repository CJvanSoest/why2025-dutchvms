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

/* See display_backlight_client.h. Sends a one-byte percent value to the
 * C6's display-backlight PWM channel over the esp-hosted custom_data
 * channel -- same transport lora_proto_client.c uses, but fire-and-forget:
 * no response is expected, so there's no callback registration or reply
 * wait here. */

#include "display_backlight_client.h"

#include "esp_hosted.h"
#include "esp_log.h"

static char const *TAG = "display_bl_client";

/* Must match connectivity_esp_hosted/slave/main/tanmatsu/priv_events.h --
 * mirrored locally rather than shared via include path, same convention
 * lora_proto_client.c's own TANMATSU_EVENT_LORA define uses (P4 kernel and
 * C6 slave firmware are separate build targets in this monorepo). */
#define TANMATSU_EVENT_DISPLAY_BL 0x05

void bv_display_backlight_send(uint8_t percent) {
    if (percent > 100)
        percent = 100;
    esp_err_t res = esp_hosted_send_custom_data(TANMATSU_EVENT_DISPLAY_BL, &percent, 1);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send display backlight update: %s", esp_err_to_name(res));
    }
}
