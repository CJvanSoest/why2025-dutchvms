#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_common.h"
#include "esp_err.h"
#include "esp_hosted_coprocessor.h"
#include "esp_hosted_peer_data.h"
#include "esp_log.h"
#include "ir_protocol_server.h"
#include "lora_protocol_server.h"
#include "nvs_flash.h"
#include "priv_events.h"
#include "tanmatsu_hardware.h"

#include <math.h>

static const char* TAG = "tanmatsu";

// ---------- WHY backlight PWM ----------

#define WHY_PWM_BASE_CLK   LEDC_USE_XTAL_CLK
#define WHY_PWM_RESOLUTION LEDC_TIMER_8_BIT
#define WHY_PWM_FREQ_HZ    25000  // Above audible

// Sets an already-configured LEDC channel's duty from a 0-100 percent value
// (factored out of why_pwm_init_channel() so the display/keyboard-backlight
// RPC callbacks below can reuse it at runtime instead of only ever setting
// duty once at boot). Returns esp_err_t instead of using ESP_ERROR_CHECK()
// internally -- the RPC callbacks below run on the shared esp-hosted Rx
// thread, which must never abort, so failure has to be reportable to the
// caller instead of always being fatal.
static esp_err_t why_pwm_set_duty_percent(ledc_channel_t channel, int duty_percent) {
    if (duty_percent < 0) duty_percent = 0;
    if (duty_percent > 100) duty_percent = 100;
    // Panel needs a duty floor/ceiling of 10-80 (out of 256): below 10 the
    // backlight doesn't visibly light, above 80 it doesn't get meaningfully
    // brighter. A *linear* interpolation of pct across that 10-80 window
    // still looked flat near 100% on real hardware -- human perceived
    // brightness vs. LED PWM duty isn't linear, it's closer to duty^(1/2.2)
    // (the usual gamma-2.2 relationship), so equal steps in physical duty
    // read as much bigger jumps near the low end than near the ceiling.
    // Apply the inverse (pct^2.2) so equal steps in the 0-100 input read as
    // equal steps to the eye across the whole range.
    uint32_t duty;
    if (duty_percent == 0) {
        duty = 0;
    } else {
        const uint32_t floor = 10, ceil = 80;
        float gamma = powf((float)duty_percent / 100.0f, 2.2f);
        duty         = floor + (uint32_t)((ceil - floor) * gamma + 0.5f);
    }
    esp_err_t res = ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
    if (res != ESP_OK) {
        return res;
    }
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
}

// Backlight PWM is a nice-to-have peripheral, not a boot-critical one --
// a transient LEDC init failure here must not abort the whole C6
// co-processor and take WiFi/BT/LoRa down with it. Returns esp_err_t so
// the caller can log and keep booting instead.
static esp_err_t why_pwm_init_channel(int gpio_num, ledc_channel_t channel, int duty_percent) {
    ledc_timer_config_t timer_conf = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = WHY_PWM_RESOLUTION,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = WHY_PWM_FREQ_HZ,
        .clk_cfg         = WHY_PWM_BASE_CLK,
    };
    esp_err_t res = ledc_timer_config(&timer_conf);
    if (res != ESP_OK) {
        return res;
    }

    ledc_channel_config_t channel_conf = {
        .gpio_num   = gpio_num,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = channel,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    res = ledc_channel_config(&channel_conf);
    if (res != ESP_OK) {
        return res;
    }

    return why_pwm_set_duty_percent(channel, duty_percent);
}

// ---------- Display backlight RPC (task #38) ----------
// P4-side sender: badgevms/drivers/display_backlight_client.c
// (bv_display_backlight_send() -> esp_hosted_send_custom_data()). Confirmed
// via KiCad (badgeCarrierCard > connectivity sheet): display backlight =
// GPIO15 = LEDC_CHANNEL_0 (BSP_DISPLAY_BL_GPIO); keyboard backlight =
// GPIO10 = LEDC_CHANNEL_1 (BSP_KEYBOARD_BL_GPIO) -- confirms
// tanmatsu_hardware.h's assignment and corrects an earlier, swapped
// schematic-trace note in badgevms/drivers/st7703.c.
static void display_bl_protocol_packet_callback(uint32_t msg_id, const uint8_t* data, size_t data_len) {
    if (msg_id != TANMATSU_EVENT_DISPLAY_BL) {
        ESP_LOGW(TAG, "Received message with unexpected ID: %d", msg_id);
        return;
    }
    if (data_len < 1) {
        ESP_LOGW(TAG, "Display backlight message too short (%d bytes)", (int)data_len);
        return;
    }
    esp_err_t res = why_pwm_set_duty_percent(LEDC_CHANNEL_0, data[0]);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "why_pwm_set_duty_percent failed: %s", esp_err_to_name(res));
    }
}

// ---------- Keyboard backlight RPC (why2025-apps#1) ----------
// P4-side sender: badgevms/drivers/keyboard_backlight_client.c
// (bv_keyboard_set_backlight() -> esp_hosted_send_custom_data()).
static void keyboard_bl_protocol_packet_callback(uint32_t msg_id, const uint8_t* data, size_t data_len) {
    if (msg_id != TANMATSU_EVENT_KEYBOARD_BL) {
        ESP_LOGW(TAG, "Received message with unexpected ID: %d", msg_id);
        return;
    }
    if (data_len < 1) {
        ESP_LOGW(TAG, "Keyboard backlight message too short (%d bytes)", (int)data_len);
        return;
    }
    esp_err_t res = why_pwm_set_duty_percent(LEDC_CHANNEL_1, data[0]);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "why_pwm_set_duty_percent failed: %s", esp_err_to_name(res));
    }
}

// ---------- LoRa SPI bus + TXEN switch ----------

static esp_err_t spi_initialize(void) {
    spi_bus_config_t buscfg = {
        .miso_io_num   = BSP_LORA_MISO,
        .mosi_io_num   = BSP_LORA_MOSI,
        .sclk_io_num   = BSP_LORA_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    return spi_bus_initialize(BSP_LORA_BUS, &buscfg, SPI_DMA_CH_AUTO);
}

// WHY badge TX-enable switch on GPIO3 — must be HIGH for LoRa TX path.
// SX126x driver in this firmware has no explicit TX/RX-switch hook, so we
// set HIGH at boot and leave it: LoRa-TX is always armed.
static esp_err_t why_lora_txen_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BSP_LORA_TXEN,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t res = gpio_config(&cfg);
    if (res != ESP_OK) return res;
    return gpio_set_level(BSP_LORA_TXEN, 1);
}

// ---------- Echo callback (Tanmatsu test channel) ----------

static void echo_protocol_packet_callback(uint32_t msg_id, const uint8_t* data, size_t data_len) {
    if (msg_id != TANMATSU_EVENT_ECHO) {
        ESP_LOGW(TAG, "Received message with unexpected ID: %d", msg_id);
        return;
    }
    esp_err_t res = esp_hosted_send_custom_data(msg_id, data, data_len);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send echo event: %s", esp_err_to_name(res));
    }
}

// ---------- Main ----------

void app_main(void) {
    // NVS first (esp-hosted needs it)
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    ESP_ERROR_CHECK(res);

    gpio_install_isr_service(0);

    // WHY backlight comes up immediately so user sees the screen during boot
    esp_err_t bl_res = why_pwm_init_channel(BSP_DISPLAY_BL_GPIO, LEDC_CHANNEL_0, 10);
    if (bl_res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init display backlight PWM: %s", esp_err_to_name(bl_res));
    }
    bl_res = why_pwm_init_channel(BSP_KEYBOARD_BL_GPIO, LEDC_CHANNEL_1, 10);
    if (bl_res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init keyboard backlight PWM: %s", esp_err_to_name(bl_res));
    }

    // esp-hosted core: SDIO/SPI slave for Wi-Fi/BT/RPC + custom_data channel
    esp_hosted_coprocessor_init();

    res = esp_hosted_register_custom_callback(TANMATSU_EVENT_ECHO, echo_protocol_packet_callback);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register echo protocol callback: %s", esp_err_to_name(res));
    }

    res = esp_hosted_register_custom_callback(TANMATSU_EVENT_DISPLAY_BL, display_bl_protocol_packet_callback);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register display backlight callback: %s", esp_err_to_name(res));
    }

    res = esp_hosted_register_custom_callback(TANMATSU_EVENT_KEYBOARD_BL, keyboard_bl_protocol_packet_callback);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register keyboard backlight callback: %s", esp_err_to_name(res));
    }

    // IR is not wired on WHY badge — skip if BSP_IR_TX is sentinel
#if BSP_IR_TX >= 0
    res = ir_initialize();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize IR: %s", esp_err_to_name(res));
    }
#else
    ESP_LOGI(TAG, "Skipping IR init (no IR-TX pad on WHY badge)");
#endif

    // LoRa: TX-enable switch HIGH, then SPI bus up, then start protocol task
    res = why_lora_txen_init();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init LoRa TXEN: %s", esp_err_to_name(res));
    }

    res = spi_initialize();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(res));
        return;  // Can't start LoRa without working SPI bus
    }

    start_lora_task();
}
