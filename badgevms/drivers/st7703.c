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

#include "st7703.h"

#include "driver/gpio.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7703.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"

#define TAG "st7703"

#define LCD_IO_RST             (GPIO_NUM_17) // -1 if not used
#define PIN_NUM_BK_LIGHT       (-1)          // -1 if not used
#define LCD_BK_LIGHT_ON_LEVEL  (1)
#define LCD_BK_LIGHT_OFF_LEVEL !LCD_BK_LIGHT_ON_LEVEL

/* --- Backlight brightness (task #31) ---------------------------------------
 * TODO CONFIRM PIN. Schematic research (2026-07-10, src/hardware/Carrier):
 * the LCD backlight *is* dimmable in hardware — display.kicad_sch has a real
 * AP3032 boost-converter (U16) driving the panel's LED string, and its CTRL
 * (dimming) pin is wired to a hierarchical net "BL_PWM" (root-level alias
 * "DISPLAY_BL"). Tracing that net at the badgeCarrierCard.kicad_sch root
 * (via the sheet's own wires/labels, not just coordinate guesswork) shows it
 * is fed by "DISPLAY_PWM" from the "Connectivity (C6 + LoRa)" sub-sheet,
 * which lands — through series resistor R23 — directly on GPIO10 of U4, the
 * ESP32-C6-WROOM-1 co-processor (connectivity.kicad_sch). That pin is *not*
 * on the ESP32-P4 this firmware runs on, so it cannot be driven with a local
 * gpio_set_level()/LEDC channel the way LCD_IO_RST (P4 GPIO17, confirmed via
 * the DSI.RESET net) is. A DNP (not-populated) 0R jumper, R16, shows there
 * was also an option to tie a local P4 pin (GPIO1) onto this same net, but
 * it isn't stuffed on production boards, so that path is open/unconfirmed
 * too. Bottom line: no local P4 GPIO is confirmed to control the backlight
 * today. Real dimming would need a command sent to the C6 (e.g. over the
 * existing P4<->C6 ESP-Hosted link) — out of scope here. Leaving this at -1
 * (matching PIN_NUM_BK_LIGHT above) is the safe choice: it documents the
 * finding instead of guessing a P4 pin number that would silently toggle an
 * unrelated, possibly-live GPIO. Only ever change this to a real gpio_num_t
 * after physically confirming a LOCAL P4 pin drives the backlight. */
#define BADGE_BACKLIGHT_GPIO (-1) /* TODO CONFIRM PIN — see comment above */

#if BADGE_BACKLIGHT_GPIO >= 0
#include "driver/ledc.h"
#define BACKLIGHT_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BACKLIGHT_LEDC_TIMER   LEDC_TIMER_2
#define BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_2
#define BACKLIGHT_LEDC_RES_BIT LEDC_TIMER_8_BIT
#define BACKLIGHT_LEDC_FREQ_HZ 5000
#endif

#define MIPI_DSI_PHY_PWR_LDO_CHAN       (3) // LDO_VO3 is connected to VDD_MIPI_DPHY
#define MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV (2500)
#define LCD_MIPI_DSI_BUS_ID             (0)
#define LCD_MIPI_DSI_LANE_NUM           (2)    // 2 data lanes
#define LCD_MIPI_DSI_LANE_BITRATE_MBPS  (1000) // 1Gbps

#ifdef CUSTOM_INIT_CMDS
static st7703_lcd_init_cmd_t const custom_init[] = CUSTOM_INIT_CMDS();
#endif // CUSTOM_INIT_CMDS

static void (*refresh_cb)(void *user_data);

typedef struct {
    lcd_device_t           device;
    esp_lcd_panel_handle_t disp_panel;
} st7703_device_t;

IRAM_ATTR static bool
    on_refresh_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx) {
    ESP_DRAM_LOGV(DRAM_STR("st7703"), "on_refresh_done");
    if (refresh_cb) {
        refresh_cb(user_ctx);
    }
    return true;
}

IRAM_ATTR static bool
    on_color_trans_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx) {
    ESP_DRAM_LOGV(DRAM_STR("st7703"), "on_color_trans_done");
    return true;
}

static esp_err_t enable_dsi_phy_power(void) {
    // Turn on the power for MIPI DSI PHY, so it can go from "No Power" state to "Shutdown" state
    static esp_ldo_channel_handle_t phy_pwr_chan = NULL;
    esp_ldo_channel_config_t        ldo_cfg      = {
                    .chan_id    = MIPI_DSI_PHY_PWR_LDO_CHAN,
                    .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr_chan));
    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");

    return ESP_OK;
}

static void panel_init(st7703_device_t *device) {
    ESP_LOGI(TAG, "Enable MIPI DSI PHY power");
    ESP_ERROR_CHECK(enable_dsi_phy_power());

    ESP_LOGI(TAG, "Create MIPI DSI bus");
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_config   = {
          .bus_id             = LCD_MIPI_DSI_BUS_ID,
          .num_data_lanes     = LCD_MIPI_DSI_LANE_NUM,
          .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
          .lane_bit_rate_mbps = LCD_MIPI_DSI_LANE_BITRATE_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

    ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
    esp_lcd_panel_io_handle_t io         = NULL;
    // we use DBI interface to send LCD commands and parameters
    esp_lcd_dbi_io_config_t   dbi_config = {
          .virtual_channel = 0,
          .lcd_cmd_bits    = 8, // according to the LCD spec
          .lcd_param_bits  = 8, // according to the LCD spec
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io));

    ESP_LOGI(TAG, "Install ST7703 LCD control panel");
    esp_lcd_dpi_panel_config_t dpi_config = ST7703_720_720_PANEL_60HZ_DPI_CONFIG();

    st7703_vendor_config_t vendor_config = {
        .mipi_config =
            {
                .dsi_bus    = mipi_dsi_bus,
                .dpi_config = &dpi_config,
            },
#ifdef CUSTOM_INIT_CMDS
        .init_cmds      = custom_init,
        .init_cmds_size = sizeof(custom_init) / sizeof(st7703_lcd_init_cmd_t),
#endif // CUSTOM_INIT_CMDS
    };

    esp_lcd_panel_dev_config_t lcd_dev_config = {
        .bits_per_pixel          = (FRAMEBUFFER_BPP * 8),
        // We are actually sending RGB data to the display. For soms reason on the WHY2025 badge
        // this is backwards
        .rgb_ele_order           = LCD_RGB_ELEMENT_ORDER_BGR,
        .reset_gpio_num          = LCD_IO_RST,
        .vendor_config           = &vendor_config,
        .flags.reset_active_high = 1,
    };

    device->disp_panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7703(io, &lcd_dev_config, &device->disp_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(device->disp_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(device->disp_panel));

    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_refresh_done = on_refresh_done,
        // .on_color_trans_done = on_color_trans_done,
    };
    esp_lcd_dpi_panel_register_event_callbacks(device->disp_panel, &cbs, NULL);

#if PIN_NUM_BK_LIGHT >= 0
    ESP_LOGI(TAG, "Turn on LCD backlight");
    gpio_set_level(PIN_NUM_BK_LIGHT, LCD_BK_LIGHT_ON_LEVEL);
#endif

#if CONFIG_LCD_TOUCH_CONTROLLER_GT911
    esp_lcd_touch_handle_t tp_handle = create_tp_handle();
#endif // CONFIG_LCD_TOUCH_CONTROLLER_GT911
}

void draw(void *dev, int x, int y, int w, int h, void *pixels) {
    st7703_device_t *device = dev;
    esp_lcd_panel_draw_bitmap(device->disp_panel, x, y, w, h, pixels);
}

void get_framebuffer(void *dev, int num, void **pixels) {
    void *fb0;
#if DISPLAY_FRAMEBUFFERS > 1
    void *fb1;
#endif
#if DISPLAY_FRAMEBUFFERS > 2
    void *fb2;
#endif
    st7703_device_t *device = dev;
#if DISPLAY_FRAMEBUFFERS == 1
    esp_lcd_dpi_panel_get_frame_buffer(device->disp_panel, DISPLAY_FRAMEBUFFERS, &fb0);
#elif DISPLAY_FRAMEBUFFERS == 2
    esp_lcd_dpi_panel_get_frame_buffer(device->disp_panel, DISPLAY_FRAMEBUFFERS, &fb0, &fb1);
#elif DISPLAY_FRAMEBUFFERS == 3
    esp_lcd_dpi_panel_get_frame_buffer(device->disp_panel, DISPLAY_FRAMEBUFFERS, &fb0, &fb1, &fb2);
#endif

    if (num == 0)
        *pixels = fb0;
#if DISPLAY_FRAMEBUFFERS > 1
    if (num == 1)
        *pixels = fb1;
#endif
#if DISPLAY_FRAMEBUFFERS > 2
    if (num == 2)
        *pixels = fb2;
#endif
}

/* --- Backlight brightness API (task #31) -----------------------------------
 * Kept as free functions (not on lcd_device_t's vtable) since there is only
 * ever one LCD on this hardware — same reasoning as e.g. bv_led_matrix_*
 * addressing a single shared panel directly rather than through a device
 * handle. `percent` is stored regardless of BADGE_BACKLIGHT_GPIO so the
 * value round-trips through NVS correctly even while no confirmed local pin
 * exists (see the comment above); the LEDC duty cycle is only ever touched
 * when BADGE_BACKLIGHT_GPIO >= 0. */
static uint8_t backlight_percent = 100;
#if BADGE_BACKLIGHT_GPIO >= 0
static bool backlight_ledc_ready = false;

static void backlight_ledc_init(void) {
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = BACKLIGHT_LEDC_MODE,
        .duty_resolution = BACKLIGHT_LEDC_RES_BIT,
        .timer_num       = BACKLIGHT_LEDC_TIMER,
        .freq_hz         = BACKLIGHT_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "backlight: ledc_timer_config failed");
        return;
    }

    ledc_channel_config_t chan_cfg = {
        .gpio_num   = BADGE_BACKLIGHT_GPIO,
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .channel    = BACKLIGHT_LEDC_CHANNEL,
        .timer_sel  = BACKLIGHT_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    if (ledc_channel_config(&chan_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "backlight: ledc_channel_config failed");
        return;
    }

    backlight_ledc_ready = true;
}
#endif

void st7703_set_brightness(uint8_t percent) {
    if (percent > 100)
        percent = 100;
    backlight_percent = percent;

#if BADGE_BACKLIGHT_GPIO >= 0
    if (!backlight_ledc_ready)
        backlight_ledc_init();
    if (backlight_ledc_ready) {
        uint32_t max_duty = (1u << BACKLIGHT_LEDC_RES_BIT) - 1;
        uint32_t duty     = (max_duty * percent) / 100;
        ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, duty);
        ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
    }
#else
    ESP_LOGI(
        TAG,
        "set_brightness(%u%%): no confirmed local backlight GPIO (BADGE_BACKLIGHT_GPIO=-1); "
        "value stored but hardware unaffected, see comment near the top of this file",
        (unsigned)percent
    );
#endif
}

uint8_t st7703_get_brightness(void) {
    return backlight_percent;
}

void set_refresh_cb(void *dev, void *user_data, void (*callback)(void *user_data)) {
    st7703_device_t *device = dev;

    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_refresh_done     = on_refresh_done,
        .on_color_trans_done = NULL,
    };

    refresh_cb = callback;
    esp_lcd_dpi_panel_register_event_callbacks(device->disp_panel, &cbs, user_data);
}

device_t *st7703_create() {
    ESP_LOGI(TAG, "Initializing");
    st7703_device_t *dev      = calloc(1, sizeof(st7703_device_t));
    device_t        *base_dev = (device_t *)dev;
    lcd_device_t    *lcd_dev  = (lcd_device_t *)dev;

    lcd_dev->_draw           = draw;
    lcd_dev->_getfb          = get_framebuffer;
    lcd_dev->_set_refresh_cb = set_refresh_cb;

    base_dev->type   = DEVICE_TYPE_LCD;
    base_dev->_open  = NULL;
    base_dev->_close = NULL;
    base_dev->_write = NULL;
    base_dev->_read  = NULL;
    base_dev->_lseek = NULL;

    panel_init(dev);

    return (device_t *)dev;
}
