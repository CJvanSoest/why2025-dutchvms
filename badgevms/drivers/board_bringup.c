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

/* Board-level bring-up that doesn't belong to the generic I2C bus VFS
 * (badgevms_i2c_bus.c): default the vibration motor off, then probe I2C2 for
 * the PCA9698 LED-matrix add-on and start its driver (led_matrix_pca9698.c)
 * plus the WS2812 status-LED task (status_led_ws2812.c) if found. Split out
 * of badgevms_i2c_bus.c, which used to mix all of this in with the generic
 * bus driver. */

#include "board_bringup.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_matrix_pca9698.h"
#include "status_led_ws2812.h"

#define TAG "board_bringup"

static void i2c2_verify_task(void *arg) {
    (void)arg;

    /* Vibration motor (GPIO3 -> R49 -> PWM_VIB -> BC847 -> motor, see
     * src/hardware/Carrier/Vibrator.kicad_sch): R49 is now a permanent wire
     * bridge on the physical board (CJ's hardware rework), so this net is no
     * longer floating/unpopulated - firmware must actively hold it LOW or
     * the pin's default/idle state (this GPIO doubles as the LED-matrix
     * add-on's INT line below, which idles pulled up) leaves the driver
     * transistor biased on and the motor spinning continuously. Done first,
     * before the 3s delay below, to minimize how long it runs unwanted at
     * boot. No app-level motor control exists yet (backlog item); this is
     * just "default off". */
    /* gpio_reset_pin() first: plain gpio_config() alone was proven
     * insufficient during the vib-test investigation (motor stayed
     * unresponsive to gpio_set_level() until this was added) - GPIO3
     * apparently needs to be explicitly detached from its default IOMUX
     * function before a plain GPIO config actually takes hold of the pad. */
    gpio_reset_pin(GPIO_NUM_3);
    gpio_config_t vib_ctl = {
        .pin_bit_mask = 1ULL << 3,
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&vib_ctl);
    gpio_set_level(3, 0);

    vTaskDelay(pdMS_TO_TICKS(3000));
    // Module/LED-matrix pinout: I2C2.SDA=GPIO22, I2C2.SCL=GPIO9.
    // PCA9698 control (LED_MATRIX J2 blue annotations): RESET=GPIO5 (active-low),
    // OE=GPIO8 (active-low, LED output enable), INT=GPIO3, WS-DATA=GPIO7.
    // Release RESET (GPIO5 HIGH); also drive OE LOW so outputs are enabled.
    gpio_config_t ctl = {
        .pin_bit_mask = (1ULL << 5) | (1ULL << 8),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE
    };
    gpio_config(&ctl);
    gpio_set_level(5, 1);          // RESET high = released (was wrongly GPIO1 before)
    gpio_set_level(8, 0);          // OE low = outputs enabled (harmless for i2c)
    vTaskDelay(pdMS_TO_TICKS(20)); // let the PCA9698 come out of reset
    ESP_LOGW(TAG, "=== I2C2 VERIFY (RESET=GPIO5 HIGH, OE=GPIO8 LOW): SDA=GPIO22 SCL=GPIO9 ===");
    int const     pairs[][2] = {{22, 9}, {9, 22}};
    uint8_t const addrs[]    = {0x40, 0x20};
    int           found      = 0;
    for (int p = 0; p < 2 && !found; p++) {
        for (size_t a = 0; a < sizeof(addrs) && !found; a++) {
            int sda = pairs[p][0], scl = pairs[p][1];
            int r1 = bb_pca_readback(sda, scl, addrs[a], 0x55);
            int r2 = (r1 == 0x55) ? bb_pca_readback(sda, scl, addrs[a], 0xAA) : -1;
            ESP_LOGW(
                TAG,
                "  SDA=GPIO%d SCL=GPIO%d @0x%02x: rb(0x55)=0x%02x rb(0xAA)=0x%02x",
                sda,
                scl,
                addrs[a],
                r1 & 0xFF,
                r2 & 0xFF
            );
            if (r1 == 0x55 && r2 == 0xAA) {
                ESP_LOGW(TAG, "  *** PCA9698 CONFIRMED: SDA=GPIO%d SCL=GPIO%d addr7=0x%02x ***", sda, scl, addrs[a]);
                found = 1;
            }
        }
    }
    if (!found)
        ESP_LOGW(TAG, "  still no echo (RESET=GPIO5 high) -> chip solder or OTHER");
    ESP_LOGW(TAG, "=== I2C2 VERIFY done ===");
    if (found) {
        led_matrix_pca9698_start();
        status_led_ws2812_start();
    }
    vTaskDelete(NULL);
}

void board_bringup_start(void) {
    xTaskCreate(i2c2_verify_task, "ledmtx", 4096, NULL, 4, NULL);
}
