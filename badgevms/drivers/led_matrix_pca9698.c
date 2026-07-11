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

/* PCA9698 LED-matrix add-on driver (12 rows x 20 cols, monochrome/on-off).
 * Split out of badgevms_i2c_bus.c, which now only holds the generic I2C bus
 * VFS layer -- see drivers/board_bringup.c for presence detection and
 * drivers/led_matrix_internal.h for the app-facing control surface this
 * exposes (led_matrix_clear/pixel/row/fill/brightness, bv_mtx_app_control). */

#include "led_matrix_pca9698.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "led_matrix_internal.h"

#include <stdbool.h>

#define TAG "led_matrix_pca9698"

// ---- PCA9698 bit-bang I2C primitives (used both for presence-probing and,
// as a fallback, for the multiplex refresh itself when the HW i2c peripheral
// on I2C_NUM_1 isn't available) ----
#define BB_DLY() esp_rom_delay_us(8)
static void bb_cfg_od(int sda, int scl) {
    gpio_config_t od = {
        .pin_bit_mask = (1ULL << sda) | (1ULL << scl),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE
    };
    gpio_config(&od);
}
static void bb_release(int sda, int scl) {
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << sda) | (1ULL << scl),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE
    };
    gpio_config(&in);
}
static void bb_start(int sda, int scl) {
    gpio_set_level(sda, 1);
    gpio_set_level(scl, 1);
    BB_DLY();
    gpio_set_level(sda, 0);
    BB_DLY();
    gpio_set_level(scl, 0);
    BB_DLY();
}
static void bb_stop(int sda, int scl) {
    gpio_set_level(sda, 0);
    BB_DLY();
    gpio_set_level(scl, 1);
    BB_DLY();
    gpio_set_level(sda, 1);
    BB_DLY();
}
static bool bb_wr(int sda, int scl, uint8_t b) {
    for (int i = 0; i < 8; i++) {
        gpio_set_level(sda, (b & 0x80) ? 1 : 0);
        b <<= 1;
        BB_DLY();
        gpio_set_level(scl, 1);
        BB_DLY();
        gpio_set_level(scl, 0);
        BB_DLY();
    }
    gpio_set_level(sda, 1);
    BB_DLY();
    gpio_set_level(scl, 1);
    BB_DLY();
    int ack = (gpio_get_level(sda) == 0);
    gpio_set_level(scl, 0);
    BB_DLY();
    return ack;
}
static uint8_t bb_rd(int sda, int scl, bool ack) {
    uint8_t b = 0;
    gpio_set_level(sda, 1);
    for (int i = 0; i < 8; i++) {
        BB_DLY();
        gpio_set_level(scl, 1);
        BB_DLY();
        b = (uint8_t)((b << 1) | (gpio_get_level(sda) & 1));
        gpio_set_level(scl, 0);
    }
    gpio_set_level(sda, ack ? 0 : 1);
    BB_DLY();
    gpio_set_level(scl, 1);
    BB_DLY();
    gpio_set_level(scl, 0);
    BB_DLY();
    gpio_set_level(sda, 1);
    return b;
}
int bb_pca_readback(int sda, int scl, uint8_t addr7, uint8_t val) {
    bb_cfg_od(sda, scl);
    bb_start(sda, scl);
    if (!bb_wr(sda, scl, (uint8_t)((addr7 << 1) | 0))) {
        bb_stop(sda, scl);
        bb_release(sda, scl);
        return -1;
    }
    if (!bb_wr(sda, scl, 0x08)) {
        bb_stop(sda, scl);
        bb_release(sda, scl);
        return -1;
    }
    if (!bb_wr(sda, scl, val)) {
        bb_stop(sda, scl);
        bb_release(sda, scl);
        return -1;
    }
    bb_stop(sda, scl);
    bb_start(sda, scl);
    if (!bb_wr(sda, scl, (uint8_t)((addr7 << 1) | 0))) {
        bb_stop(sda, scl);
        bb_release(sda, scl);
        return -1;
    }
    if (!bb_wr(sda, scl, 0x08)) {
        bb_stop(sda, scl);
        bb_release(sda, scl);
        return -1;
    }
    bb_start(sda, scl);
    if (!bb_wr(sda, scl, (uint8_t)((addr7 << 1) | 1))) {
        bb_stop(sda, scl);
        bb_release(sda, scl);
        return -1;
    }
    uint8_t r = bb_rd(sda, scl, false);
    bb_stop(sda, scl);
    bb_release(sda, scl);
    return (int)r;
}
/* ---- PCA9698 LED-matrix driver (bit-bang) ----
 * Mapping (from LED_MATRIX.kicad_sch, confirmed via geometry):
 *   rows PB0..PB11 (anodes, drive HIGH) = global IO bit 0..11
 *      = OP0 bit0..7 (PB0..PB7), OP1 bit0..3 (PB8..PB11)
 *   cols PA0..PA19 (cathodes, sink LOW) = global IO bit 12..31
 *      = OP1 bit4..7 (PA0..PA3), OP2 bit0..7 (PA4..PA11), OP3 bit0..7 (PA12..PA19)
 *   IO4 (OP4) unused.
 * Each row has its own 1k series resistor (3x R_Pack04 = 12) -> a row sources
 * ~2.7mA total, split over its lit columns. So we ALWAYS drive exactly one row
 * HIGH at a time (multiplex); every PCA pin then stays well under spec. */
static bool bb_write_regs(int sda, int scl, uint8_t addr7, uint8_t reg, uint8_t const *data, int n) {
    bb_cfg_od(sda, scl);
    bb_start(sda, scl);
    bool ok = bb_wr(sda, scl, (uint8_t)((addr7 << 1) | 0));
    ok      = ok && bb_wr(sda, scl, (uint8_t)(reg | 0x80)); /* bit7 = auto-increment */
    for (int i = 0; i < n && ok; i++) ok = ok && bb_wr(sda, scl, data[i]);
    bb_stop(sda, scl);
    bb_release(sda, scl);
    return ok;
}
/* Build the 5 output-port bytes for one scanned row.
 * Cols PA = anodes (drive HIGH to light); rows PB = cathodes (active row LOW,
 * all other rows HIGH = off). col_on = 20-bit mask of LEDs to light in this row.
 *
 * col_on bit c is API column c (0 = leftmost, per badgevms/led_matrix.h and
 * every app built against it - e.g. cj_ledmatrix's text scroller, whose
 * on-screen preview mirror and gfont5x7 glyph-column extraction were both
 * double-checked and are internally consistent with "increasing column index
 * = further right"). The kicad-geometry comment above only confirmed which
 * global IO bits are the PA (col) bank vs the PB (row) bank and their
 * polarity - it never pinned down which physical end of the PA0..PA19 run is
 * left vs right on the mounted panel. That direction was in fact backwards:
 * PA0..PA19 run right-to-left, so API column c lives at PA(19-c), not PA(c).
 * Solid/border/checkerboard/wipe patterns don't reveal a pure horizontal
 * mirror at a glance (checkerboard just looks phase-shifted, a symmetric
 * border is unaffected, a sweeping bar just seems to move the "wrong" way
 * without a reference), which is why this only became obvious once
 * human-readable scrolling text came out mirrored. */
static void mtx_build_op(uint8_t op[5], int row, uint32_t col_on) {
    op[0] = 0xFF; /* PB0-7 rows off (high) */
    op[1] = 0x0F; /* PB8-11 off (low nibble high), PA0-3 off (high nibble low) */
    op[2] = 0x00; /* PA4-11 off (low) */
    op[3] = 0x00; /* PA12-19 off (low) */
    op[4] = 0x00; /* unused */
    if (row >= 0 && row < 8)
        op[0] &= (uint8_t) ~(1u << row); /* active row -> LOW */
    else if (row >= 8 && row < 12)
        op[1] &= (uint8_t) ~(1u << (row - 8));
    for (int c = 0; c < 20; c++) {
        if (col_on & (1u << c)) {
            int pa     = 19 - c; /* API col c -> physical PA(19-c), see comment above */
            int g      = 12 + pa;
            op[g / 8] |= (uint8_t)(1u << (g % 8)); /* lit col -> HIGH */
        }
    }
}
#define MTX_ADDR 0x20
#define MTX_ROWS 12
#define MTX_COLS 20
/* Polarity (confirmed on hardware): cols PA are ANODES (HIGH = lit), rows PB are
 * CATHODES (the active row is driven LOW; inactive rows HIGH = off).
 * Blank = no active row (all rows HIGH) + all cols LOW = fully dark. Written
 * before each row so the non-atomic OP update can't ghost the previous row. */
static uint8_t const mtx_blank[5] = {0xFF, 0x0F, 0x00, 0x00, 0x00};
/* ---- Framebuffer + background multiplex refresh ---- */
static uint32_t      mtx_fb[MTX_ROWS]; /* bit c set in row r = LED (r,c) on */
static int           mtx_sda = 22, mtx_scl = 9;

/* Set by bv_led_matrix_take()/bv_led_matrix_release() (see
 * drivers/led_matrix_internal.h + led_matrix_bridge.c): while true, an app
 * owns mtx_fb and mtx_demo_task below must not touch it. The multiplex
 * refresh task (mtx_refresh_task_hw/bb) keeps running unconditionally either
 * way — it only reads mtx_fb, so the app's last-drawn frame stays on screen. */
bool volatile bv_mtx_app_control = false;

void led_matrix_clear(void) {
    for (int i = 0; i < MTX_ROWS; i++) mtx_fb[i] = 0;
}
void led_matrix_pixel(int r, int c, bool on) {
    if ((unsigned)r >= MTX_ROWS || (unsigned)c >= MTX_COLS)
        return;
    if (on)
        mtx_fb[r] |= (1u << c);
    else
        mtx_fb[r] &= ~(1u << c);
}
void led_matrix_row(int r, uint32_t mask) {
    if ((unsigned)r < MTX_ROWS)
        mtx_fb[r] = mask & 0xFFFFFu;
}
void led_matrix_fill(bool on) {
    uint32_t m = on ? 0xFFFFFu : 0u;
    for (int i = 0; i < MTX_ROWS; i++) mtx_fb[i] = m;
}

/* Global brightness via PWM on the active-low OE pin (GPIO8): outputs are
 * enabled while OE is LOW, so brightness% = low-duty fraction. */
#define MTX_OE_GPIO 8
static int mtx_brightness = 100;
void       led_matrix_brightness(int pct) {
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    mtx_brightness = pct;
    uint32_t duty  = (uint32_t)(255 * (100 - pct) / 100); /* HIGH (=OE off) fraction */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
static void mtx_oe_pwm_init(int pct) {
    ledc_timer_config_t t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 4000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num   = MTX_OE_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&c);
    led_matrix_brightness(pct);
}

/* Refresh backends multiplex mtx_fb onto the panel one row at a time (the row
 * stays latched until the next write). Prefer the hardware i2c peripheral: each
 * row write blocks on an ISR semaphore, so the CPU is free and idle/WDT are fed
 * WITHOUT an artificial blanking delay -> no flicker (~460 Hz). Bit-bang is a
 * fallback; it busy-waits so it needs a per-frame yield (visible flicker). */
static i2c_bus_handle_t        mtx_bus = NULL;
static i2c_bus_device_handle_t mtx_dev = NULL;

static bool mtx_hw_init(void) {
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = mtx_sda,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_io_num       = mtx_scl,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    mtx_bus = i2c_bus_create(I2C_NUM_1, &cfg);
    if (!mtx_bus)
        return false;
    mtx_dev = i2c_bus_device_create(mtx_bus, MTX_ADDR, 400000);
    if (!mtx_dev)
        return false;
    uint8_t const ioc_out[5] = {0, 0, 0, 0, 0}; /* all banks -> outputs */
    return i2c_bus_write_bytes(mtx_dev, 0x18 | 0x80, 5, (uint8_t *)ioc_out) == ESP_OK;
}

static void mtx_refresh_task_hw(void *arg) {
    (void)arg;
    ESP_LOGW(TAG, "=== LED-matrix refresh (HW i2c) START ===");
    for (;;) {
        for (int r = 0; r < MTX_ROWS; r++) {
            uint8_t op[5];
            mtx_build_op(op, r, mtx_fb[r]);
            i2c_bus_write_bytes(mtx_dev, 0x08 | 0x80, 5, (uint8_t *)mtx_blank); /* blank */
            i2c_bus_write_bytes(mtx_dev, 0x08 | 0x80, 5, op);                   /* then row+cols */
        }
    }
}

static void mtx_refresh_task_bb(void *arg) {
    (void)arg;
    uint8_t const ioc_out[5] = {0, 0, 0, 0, 0};
    bb_write_regs(mtx_sda, mtx_scl, MTX_ADDR, 0x18, ioc_out, 5);
    ESP_LOGW(TAG, "=== LED-matrix refresh (bit-bang fallback) START ===");
    for (;;) {
        for (int r = 0; r < MTX_ROWS; r++) {
            uint8_t op[5];
            mtx_build_op(op, r, mtx_fb[r]);
            bb_write_regs(mtx_sda, mtx_scl, MTX_ADDR, 0x08, (uint8_t *)mtx_blank, 5); /* blank */
            bb_write_regs(mtx_sda, mtx_scl, MTX_ADDR, 0x08, op, 5);                   /* then row+cols */
        }
        vTaskDelay(1); /* busy-wait backend must yield so idle + WDT are fed */
    }
}

/* Default boot content: a stable border + a pixel bouncing inside it. */
static void mtx_demo_task(void *arg) {
    (void)arg;
    int px = 5, py = 3, vx = 1, vy = 1;
    for (;;) {
        if (bv_mtx_app_control) {
            /* An app has taken control of the matrix framebuffer — leave it
             * alone and just idle (still yielding so idle/WDT are fed). */
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }
        led_matrix_clear();
        led_matrix_row(0, 0xFFFFFu);
        led_matrix_row(MTX_ROWS - 1, 0xFFFFFu);
        for (int r = 0; r < MTX_ROWS; r++) {
            led_matrix_pixel(r, 0, true);
            led_matrix_pixel(r, MTX_COLS - 1, true);
        }
        led_matrix_pixel(py, px, true);
        px += vx;
        py += vy;
        if (px <= 1 || px >= MTX_COLS - 2)
            vx = -vx;
        if (py <= 1 || py >= MTX_ROWS - 2)
            vy = -vy;
        vTaskDelay(pdMS_TO_TICKS(120));
    }
}

void led_matrix_pca9698_start(void) {
    /* Detect which pin order ACKs, then start the driver (refresh) + demo. */
    mtx_sda = 22;
    mtx_scl = 9;
    if (bb_pca_readback(9, 22, MTX_ADDR, 0x55) == 0x55) {
        mtx_sda = 9;
        mtx_scl = 22;
    }
    led_matrix_clear();
    if (mtx_hw_init()) {
        mtx_oe_pwm_init(60); /* default global brightness 60% (OE PWM) */
        xTaskCreate(mtx_refresh_task_hw, "mtxrfsh", 4096, NULL, 5, NULL);
    } else {
        ESP_LOGW(TAG, "  HW i2c (I2C_NUM_1) init failed -> bit-bang fallback");
        xTaskCreate(mtx_refresh_task_bb, "mtxrfsh", 4096, NULL, 2, NULL);
    }
    xTaskCreate(mtx_demo_task, "mtxdemo", 4096, NULL, 2, NULL);
}
