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

#include "badgevms_i2c_bus.h"

#include "badgevms_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "task.h"

#include <stdbool.h>
#include <stdio.h>

#include "badgevms/lora.h"
#include "badgevms/wifi.h"

#define SDA_PIN 18
#define SCL_PIN 20

#define I2C_MASTER_SCL_IO  SCL_PIN
#define I2C_MASTER_SDA_IO  SDA_PIN
#define I2C_MASTER_NUM     I2C_NUM_0
#define I2C_MASTER_FREQ_HZ I2C0_MASTER_FREQ_HZ
#define I2C_MASTER_TIMEOUT 100

#define TAG "badgevms_i2c_bus"

static i2c_config_t why_config = {
    .mode             = I2C_MODE_MASTER,
    .sda_io_num       = I2C_MASTER_SDA_IO,
    .sda_pullup_en    = GPIO_PULLUP_ENABLE,
    .scl_io_num       = I2C_MASTER_SCL_IO,
    .scl_pullup_en    = GPIO_PULLUP_ENABLE,
    .master.clk_speed = I2C_MASTER_FREQ_HZ,
};

typedef struct {
    i2c_bus_device_t device;
    i2c_bus_handle_t handle;
    i2c_config_t     config;
    bool             devices[255];
    char const      *name;
} badgevms_i2c_bus_device_t;

typedef struct {
    i2c_device_t               device;
    badgevms_i2c_bus_device_t *bus;
    i2c_bus_device_handle_t    handle;
    uint8_t                    address;
    uint32_t                   clk_speed;
} badgevms_i2c_device_t;

i2c_device_t *badgevms_i2c_device_create(badgevms_i2c_bus_device_t *bus, uint8_t address, uint32_t clk_speed);

static int i2c_bus_open(void *dev, path_t *path, int flags, mode_t mode) {
    if (path->directory || path->filename)
        return -1;
    return 0;
}

static int i2c_bus_close(void *dev, int fd) {
    if (fd == 0)
        return 0;
    return -1;
}

static ssize_t i2c_bus_write(void *dev, int fd, void const *buf, size_t count) {
    return 0;
}

static ssize_t i2c_bus_read(void *dev, int fd, void *buf, size_t count) {
    return 0;
}

static ssize_t i2c_bus_lseek(void *dev, int fd, off_t offset, int whence) {
    return (off_t)-1;
}

static int _i2c_bus_scan(void *dev, i2c_scanresult_t *results, int num) {
    badgevms_i2c_bus_device_t *device       = dev;
    uint8_t                    device_count = i2c_bus_scan(device->handle, (uint8_t *)results, num);
    return device_count;
}

static i2c_device_t *_i2c_device_create(void *dev, uint8_t address, uint32_t clk_speed) {
    badgevms_i2c_bus_device_t *device = dev;
    if (device->devices[address]) {
        ESP_LOGW(TAG, "%s: Device %i alreadt opened", address);
        return NULL;
    }

    return badgevms_i2c_device_create(device, address, clk_speed);
}

static int i2c_device_open(void *dev, path_t *path, int flags, mode_t mode) {
    if (path->directory || path->filename)
        return -1;
    return 0;
}

static int i2c_device_close(void *dev, int fd) {
    if (fd == 0)
        return 0;
    return -1;
}

static ssize_t i2c_device_write(void *dev, int fd, void const *buf, size_t count) {
    badgevms_i2c_device_t *device = dev;
    esp_err_t              err;

    if (count == 1) {
        err = i2c_bus_write_byte(device->handle, fd, *((uint8_t *)buf));
        if (err == ESP_OK) {
            return 1;
        }

        return 0;
    }

    err = i2c_bus_write_bytes(device->handle, fd, count, (uint8_t *)buf);
    if (err == ESP_OK) {
        return count;
    }

    return 0;
}

static ssize_t i2c_device_read(void *dev, int fd, void *buf, size_t count) {
    badgevms_i2c_device_t *device = dev;
    esp_err_t              err;

    if (count == 1) {
        err = i2c_bus_read_byte(device->handle, fd, ((uint8_t *)buf));
        if (err == ESP_OK) {
            return 1;
        }

        return 0;
    }

    err = i2c_bus_read_bytes(device->handle, fd, count, (uint8_t *)buf);
    if (err == ESP_OK) {
        return count;
    }

    return 0;
}

static ssize_t i2c_device_lseek(void *dev, int fd, off_t offset, int whence) {
    return (off_t)-1;
}

static uint8_t i2c_device_get_address(void *dev) {
    badgevms_i2c_device_t *device = dev;
    return device->address;
}

static void i2c_device_destroy(void *dev) {
    badgevms_i2c_device_t *device = dev;
    task_record_resource_free(RES_DEVICE, dev);
    i2c_bus_device_delete(device->handle);
    device->bus->devices[device->address] = false;
    free(dev);
}

i2c_device_t *badgevms_i2c_device_create(badgevms_i2c_bus_device_t *bus, uint8_t address, uint32_t clk_speed) {
    ESP_LOGI(TAG, "Creating i2c device at address %u", address);

    badgevms_i2c_device_t *dev      = calloc(1, sizeof(badgevms_i2c_device_t));
    i2c_device_t          *i2c_dev  = (i2c_device_t *)dev;
    device_t              *base_dev = (device_t *)dev;

    base_dev->type     = DEVICE_TYPE_BLOCK;
    base_dev->_open    = i2c_device_open;
    base_dev->_close   = i2c_device_close;
    base_dev->_write   = i2c_device_write;
    base_dev->_read    = i2c_device_read;
    base_dev->_lseek   = i2c_device_lseek;
    base_dev->_destroy = i2c_device_destroy;

    i2c_dev->_get_address = i2c_device_get_address;

    if (!dev) {
        ESP_LOGE(TAG, "Failed to allocate i2c device");
        return NULL;
    }

    dev->bus       = bus;
    dev->clk_speed = clk_speed;
    dev->address   = address;
    dev->handle    = i2c_bus_device_create(bus->handle, address, clk_speed);
    if (!dev->handle) {
        ESP_LOGE(TAG, "Failed to allocate i2c device");
        free(dev);
        return NULL;
    }

    task_record_resource_alloc(RES_DEVICE, dev);
    dev->bus->devices[address] = true;
    return i2c_dev;
}

// ---- Verify I2C2 / PCA9698 on the module-pinout pins: SDA=GPIO22, SCL=GPIO9 ----
#define BB_DLY() esp_rom_delay_us(8)
static void bb_cfg_od(int sda, int scl) {
    gpio_config_t od = {.pin_bit_mask = (1ULL << sda) | (1ULL << scl),
                        .mode = GPIO_MODE_INPUT_OUTPUT_OD, .pull_up_en = GPIO_PULLUP_ENABLE,
                        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&od);
}
static void bb_release(int sda, int scl) {
    gpio_config_t in = {.pin_bit_mask = (1ULL << sda) | (1ULL << scl), .mode = GPIO_MODE_INPUT,
                        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&in);
}
static void bb_start(int sda, int scl) {
    gpio_set_level(sda, 1); gpio_set_level(scl, 1); BB_DLY();
    gpio_set_level(sda, 0); BB_DLY(); gpio_set_level(scl, 0); BB_DLY();
}
static void bb_stop(int sda, int scl) {
    gpio_set_level(sda, 0); BB_DLY(); gpio_set_level(scl, 1); BB_DLY();
    gpio_set_level(sda, 1); BB_DLY();
}
static bool bb_wr(int sda, int scl, uint8_t b) {
    for (int i = 0; i < 8; i++) {
        gpio_set_level(sda, (b & 0x80) ? 1 : 0); b <<= 1; BB_DLY();
        gpio_set_level(scl, 1); BB_DLY(); gpio_set_level(scl, 0); BB_DLY();
    }
    gpio_set_level(sda, 1); BB_DLY(); gpio_set_level(scl, 1); BB_DLY();
    int ack = (gpio_get_level(sda) == 0);
    gpio_set_level(scl, 0); BB_DLY();
    return ack;
}
static uint8_t bb_rd(int sda, int scl, bool ack) {
    uint8_t b = 0;
    gpio_set_level(sda, 1);
    for (int i = 0; i < 8; i++) {
        BB_DLY(); gpio_set_level(scl, 1); BB_DLY();
        b = (uint8_t)((b << 1) | (gpio_get_level(sda) & 1));
        gpio_set_level(scl, 0);
    }
    gpio_set_level(sda, ack ? 0 : 1); BB_DLY();
    gpio_set_level(scl, 1); BB_DLY(); gpio_set_level(scl, 0); BB_DLY();
    gpio_set_level(sda, 1);
    return b;
}
static int bb_pca_readback(int sda, int scl, uint8_t addr7, uint8_t val) {
    bb_cfg_od(sda, scl);
    bb_start(sda, scl);
    if (!bb_wr(sda, scl, (uint8_t)((addr7 << 1) | 0))) { bb_stop(sda, scl); bb_release(sda, scl); return -1; }
    if (!bb_wr(sda, scl, 0x08))                        { bb_stop(sda, scl); bb_release(sda, scl); return -1; }
    if (!bb_wr(sda, scl, val))                         { bb_stop(sda, scl); bb_release(sda, scl); return -1; }
    bb_stop(sda, scl);
    bb_start(sda, scl);
    if (!bb_wr(sda, scl, (uint8_t)((addr7 << 1) | 0))) { bb_stop(sda, scl); bb_release(sda, scl); return -1; }
    if (!bb_wr(sda, scl, 0x08))                        { bb_stop(sda, scl); bb_release(sda, scl); return -1; }
    bb_start(sda, scl);
    if (!bb_wr(sda, scl, (uint8_t)((addr7 << 1) | 1))) { bb_stop(sda, scl); bb_release(sda, scl); return -1; }
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
    ok = ok && bb_wr(sda, scl, (uint8_t)(reg | 0x80)); /* bit7 = auto-increment */
    for (int i = 0; i < n && ok; i++)
        ok = ok && bb_wr(sda, scl, data[i]);
    bb_stop(sda, scl);
    bb_release(sda, scl);
    return ok;
}
/* Build the 5 output-port bytes for one scanned row.
 * Cols PA = anodes (drive HIGH to light); rows PB = cathodes (active row LOW,
 * all other rows HIGH = off). col_on = 20-bit mask of LEDs to light in this row. */
static void mtx_build_op(uint8_t op[5], int row, uint32_t col_on) {
    op[0] = 0xFF;  /* PB0-7 rows off (high) */
    op[1] = 0x0F;  /* PB8-11 off (low nibble high), PA0-3 off (high nibble low) */
    op[2] = 0x00;  /* PA4-11 off (low) */
    op[3] = 0x00;  /* PA12-19 off (low) */
    op[4] = 0x00;  /* unused */
    if (row >= 0 && row < 8)
        op[0] &= (uint8_t)~(1u << row);          /* active row -> LOW */
    else if (row >= 8 && row < 12)
        op[1] &= (uint8_t)~(1u << (row - 8));
    for (int c = 0; c < 20; c++) {
        if (col_on & (1u << c)) {
            int g = 12 + c;
            op[g / 8] |= (uint8_t)(1u << (g % 8));  /* lit col -> HIGH */
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
static uint32_t mtx_fb[MTX_ROWS];     /* bit c set in row r = LED (r,c) on */
static int      mtx_sda = 22, mtx_scl = 9;

void led_matrix_clear(void) {
    for (int i = 0; i < MTX_ROWS; i++) mtx_fb[i] = 0;
}
void led_matrix_pixel(int r, int c, bool on) {
    if ((unsigned)r >= MTX_ROWS || (unsigned)c >= MTX_COLS) return;
    if (on) mtx_fb[r] |= (1u << c);
    else    mtx_fb[r] &= ~(1u << c);
}
void led_matrix_row(int r, uint32_t mask) {
    if ((unsigned)r < MTX_ROWS) mtx_fb[r] = mask & 0xFFFFFu;
}
void led_matrix_fill(bool on) {
    uint32_t m = on ? 0xFFFFFu : 0u;
    for (int i = 0; i < MTX_ROWS; i++) mtx_fb[i] = m;
}

/* Global brightness via PWM on the active-low OE pin (GPIO8): outputs are
 * enabled while OE is LOW, so brightness% = low-duty fraction. */
#define MTX_OE_GPIO 8
static int mtx_brightness = 100;
void led_matrix_brightness(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    mtx_brightness = pct;
    uint32_t duty = (uint32_t)(255 * (100 - pct) / 100);  /* HIGH (=OE off) fraction */
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
    uint8_t const ioc_out[5] = {0, 0, 0, 0, 0};  /* all banks -> outputs */
    return i2c_bus_write_bytes(mtx_dev, 0x18 | 0x80, 5, (uint8_t *)ioc_out) == ESP_OK;
}

static void mtx_refresh_task_hw(void *arg) {
    (void)arg;
    ESP_LOGW(TAG, "=== LED-matrix refresh (HW i2c) START ===");
    for (;;) {
        for (int r = 0; r < MTX_ROWS; r++) {
            uint8_t op[5];
            mtx_build_op(op, r, mtx_fb[r]);
            i2c_bus_write_bytes(mtx_dev, 0x08 | 0x80, 5, (uint8_t *)mtx_blank);  /* blank */
            i2c_bus_write_bytes(mtx_dev, 0x08 | 0x80, 5, op);  /* then row+cols */
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
            bb_write_regs(mtx_sda, mtx_scl, MTX_ADDR, 0x08, (uint8_t *)mtx_blank, 5);  /* blank */
            bb_write_regs(mtx_sda, mtx_scl, MTX_ADDR, 0x08, op, 5);  /* then row+cols */
        }
        vTaskDelay(1);  /* busy-wait backend must yield so idle + WDT are fed */
    }
}

/* Default boot content: a stable border + a pixel bouncing inside it. */
static void mtx_demo_task(void *arg) {
    (void)arg;
    int px = 5, py = 3, vx = 1, vy = 1;
    for (;;) {
        led_matrix_clear();
        led_matrix_row(0, 0xFFFFFu);
        led_matrix_row(MTX_ROWS - 1, 0xFFFFFu);
        for (int r = 0; r < MTX_ROWS; r++) {
            led_matrix_pixel(r, 0, true);
            led_matrix_pixel(r, MTX_COLS - 1, true);
        }
        led_matrix_pixel(py, px, true);
        px += vx; py += vy;
        if (px <= 1 || px >= MTX_COLS - 2) vx = -vx;
        if (py <= 1 || py >= MTX_ROWS - 2) vy = -vy;
        vTaskDelay(pdMS_TO_TICKS(120));
    }
}

/* ---- 4x RGBW status LEDs (SK6812-style) on WS-DATA = GPIO7. RGBW means 4
 * bytes per LED (G,R,B,W) — sending only 3 bytes per LED would desync the
 * chain. Driven via the RMT peripheral. ---- */
#define WS_GPIO  7
#define WS_COUNT 4
static rmt_channel_handle_t ws_chan = NULL;
static rmt_encoder_handle_t ws_enc  = NULL;
static uint8_t              ws_grbw[WS_COUNT * 4];  /* per LED: G, R, B, W */

static bool ws2812_init(void) {
    rmt_tx_channel_config_t txc = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .gpio_num          = WS_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz     = 10000000,  /* 0.1us / tick */
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&txc, &ws_chan) != ESP_OK)
        return false;
    rmt_bytes_encoder_config_t bc = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},  /* 0.3us / 0.9us */
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 5},  /* 0.9us / 0.5us (closer to typ 0.45us) */
        .flags.msb_first = 1,
    };
    if (rmt_new_bytes_encoder(&bc, &ws_enc) != ESP_OK)
        return false;
    return rmt_enable(ws_chan) == ESP_OK;
}
static void ws2812_set(int i, uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    if ((unsigned)i >= WS_COUNT) return;
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
    ws2812_set(i, (r * LED_BRIGHTNESS) / 100, (g * LED_BRIGHTNESS) / 100,
               (b * LED_BRIGHTNESS) / 100, 0);
}

/* Status indicator on the 4 RGBW LEDs (GPIO7):
 *   LED0 = LoRa radio : green = up/active, blue = starting up, red = offline
 *   LED1 = WiFi       : green = connected, blue = enabled/connecting, off = disabled
 *   LED2, LED3        = reserved (off) for future DM/channel message-notify,
 *                       which needs an app->LED channel that does not exist yet.
 * State comes from the firmware-internal LoRa/WiFi query APIs. */
#define LORA_STARTUP_GRACE_S 15
static void ws2812_task(void *arg) {
    (void)arg;
    if (!ws2812_init()) {
        ESP_LOGW(TAG, "WS2812 init failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "=== RGBW status LEDs (4x on GPIO%d) START: LED0=radio LED1=wifi ===", WS_GPIO);
    uint32_t secs = 0;
    for (;;) {
        /* LED0: LoRa radio status */
        lora_mode_t   mode;
        lora_status_t lst;
        if (lora_get_mode(&mode) && mode != LORA_MODE_UNKNOWN) {
            ws2812_set_scaled(0, 0, 255, 0);            /* green: up/active */
        } else if (lora_get_status(&lst) && lst.chip_type != LORA_CHIP_UNKNOWN) {
            ws2812_set_scaled(0, 0, 255, 0);            /* green: radio responding */
        } else if (secs < LORA_STARTUP_GRACE_S) {
            ws2812_set_scaled(0, 0, 0, 255);            /* blue: still starting up */
        } else {
            ws2812_set_scaled(0, 255, 0, 0);            /* red: offline / no response */
        }

        /* LED1: WiFi status (Bluetooth has no status API yet) */
        if (wifi_get_status() == WIFI_DISABLED) {
            ws2812_set_scaled(1, 0, 0, 0);              /* off: disabled */
        } else if (wifi_get_connection_status() == WIFI_CONNECTED) {
            ws2812_set_scaled(1, 0, 255, 0);            /* green: connected */
        } else {
            ws2812_set_scaled(1, 0, 0, 255);            /* blue: enabled / connecting */
        }

        /* LED2, LED3: reserved for message-notify (app->LED channel TBD) */
        ws2812_set_scaled(2, 0, 0, 0);
        ws2812_set_scaled(3, 0, 0, 0);

        ws2812_show();
        vTaskDelay(pdMS_TO_TICKS(1000));
        secs += 1;
    }
}

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
    esp_err_t vib_cfg_err = gpio_config(&vib_ctl);
    gpio_set_level(3, 0);
    /* DIAG (temporary): motor kept spinning through two attempts already
     * using this exact proven-working configuration. Confirming the code
     * actually runs and what it reads back before concluding this is a
     * hardware short from the hand-soldered R49 bridge rather than firmware. */
    esp_rom_printf(
        "[vib-off] gpio_config=%d level after set_level(0)=%d\n",
        (int)vib_cfg_err,
        gpio_get_level(GPIO_NUM_3)
    );

    vTaskDelay(pdMS_TO_TICKS(3000));
    // Module/LED-matrix pinout: I2C2.SDA=GPIO22, I2C2.SCL=GPIO9.
    // PCA9698 control (LED_MATRIX J2 blue annotations): RESET=GPIO5 (active-low),
    // OE=GPIO8 (active-low, LED output enable), INT=GPIO3, WS-DATA=GPIO7.
    // Release RESET (GPIO5 HIGH); also drive OE LOW so outputs are enabled.
    gpio_config_t ctl = {.pin_bit_mask = (1ULL << 5) | (1ULL << 8), .mode = GPIO_MODE_OUTPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                         .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&ctl);
    gpio_set_level(5, 1);             // RESET high = released (was wrongly GPIO1 before)
    gpio_set_level(8, 0);             // OE low = outputs enabled (harmless for i2c)
    vTaskDelay(pdMS_TO_TICKS(20));    // let the PCA9698 come out of reset
    ESP_LOGW(TAG, "=== I2C2 VERIFY (RESET=GPIO5 HIGH, OE=GPIO8 LOW): SDA=GPIO22 SCL=GPIO9 ===");
    int const pairs[][2] = {{22, 9}, {9, 22}};
    uint8_t const addrs[] = {0x40, 0x20};
    int found = 0;
    for (int p = 0; p < 2 && !found; p++) {
        for (size_t a = 0; a < sizeof(addrs) && !found; a++) {
            int sda = pairs[p][0], scl = pairs[p][1];
            int r1 = bb_pca_readback(sda, scl, addrs[a], 0x55);
            int r2 = (r1 == 0x55) ? bb_pca_readback(sda, scl, addrs[a], 0xAA) : -1;
            ESP_LOGW(TAG, "  SDA=GPIO%d SCL=GPIO%d @0x%02x: rb(0x55)=0x%02x rb(0xAA)=0x%02x",
                     sda, scl, addrs[a], r1 & 0xFF, r2 & 0xFF);
            if (r1 == 0x55 && r2 == 0xAA) {
                ESP_LOGW(TAG, "  *** PCA9698 CONFIRMED: SDA=GPIO%d SCL=GPIO%d addr7=0x%02x ***",
                         sda, scl, addrs[a]);
                found = 1;
            }
        }
    }
    if (!found) ESP_LOGW(TAG, "  still no echo (RESET=GPIO5 high) -> chip solder or OTHER");
    ESP_LOGW(TAG, "=== I2C2 VERIFY done ===");
    if (found) {
        /* Detect which pin order ACK'd, then start the driver (refresh) + demo. */
        mtx_sda = 22; mtx_scl = 9;
        if (bb_pca_readback(9, 22, MTX_ADDR, 0x55) == 0x55) { mtx_sda = 9; mtx_scl = 22; }
        led_matrix_clear();
        if (mtx_hw_init()) {
            mtx_oe_pwm_init(60);  /* default global brightness 60% (OE PWM) */
            xTaskCreate(mtx_refresh_task_hw, "mtxrfsh", 4096, NULL, 5, NULL);
        } else {
            ESP_LOGW(TAG, "  HW i2c (I2C_NUM_1) init failed -> bit-bang fallback");
            xTaskCreate(mtx_refresh_task_bb, "mtxrfsh", 4096, NULL, 2, NULL);
        }
        xTaskCreate(mtx_demo_task, "mtxdemo", 4096, NULL, 2, NULL);
        xTaskCreate(ws2812_task, "ws2812", 4096, NULL, 2, NULL);  /* 4 side LEDs */
    }
    vTaskDelete(NULL);
}

device_t *badgevms_i2c_bus_create(char const *name, uint8_t port, uint32_t clk_speed) {
    ESP_LOGI(TAG, "Initializing");
    badgevms_i2c_bus_device_t *dev      = calloc(1, sizeof(badgevms_i2c_bus_device_t));
    i2c_bus_device_t          *i2c_bus  = (i2c_bus_device_t *)dev;
    device_t                  *base_dev = (device_t *)dev;

    base_dev->type   = DEVICE_TYPE_BUS;
    base_dev->_open  = i2c_bus_open;
    base_dev->_close = i2c_bus_close;
    base_dev->_write = i2c_bus_write;
    base_dev->_read  = i2c_bus_read;
    base_dev->_lseek = i2c_bus_lseek;

    i2c_bus->_scan          = _i2c_bus_scan;
    i2c_bus->_device_create = _i2c_device_create;

    dev->config                  = why_config;
    dev->config.master.clk_speed = clk_speed;
    dev->name                    = strdup(name);

    i2c_port_t p = I2C_NUM_0;
    switch (port) {
        case 0: p = I2C_NUM_0; break;
        case 1: p = I2C_NUM_1; break;
        default:
    }

    dev->handle = i2c_bus_create(p, &dev->config);

    if (!dev->handle) {
        ESP_LOGE(TAG, "Failed to initialize bus");
        free(dev);
        return NULL;
    }

    /* On main-bus init, bring up the LED-matrix add-on: detect the PCA9698 on
     * I2C2 (GPIO22/9), then run the matrix refresh + WS2812 status-LED tasks. */
    if (port == 0) {
        xTaskCreate(i2c2_verify_task, "ledmtx", 4096, NULL, 4, NULL);
    }

    return (device_t *)dev;
}
