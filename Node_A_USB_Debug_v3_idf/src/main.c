/*
 * Node_A_USB_Debug_v3_idf
 * USB UAC microphone (synthetic ~480 Hz square wave) using Espressif usb_device_uac.
 * Bypasses broken arduino-esp32 USBAudioCard mic path (issue #12518).
 */

#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "led_strip.h"
#include "usb_device_uac.h"

static const char *TAG = "uac_debug";

#define RGB_GPIO      48
#define RGB_LED_COUNT 1

static led_strip_handle_t s_led;
static volatile bool s_stream_active;

static void rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_led == NULL) {
        return;
    }
    led_strip_set_pixel(s_led, 0, r, g, b);
    led_strip_refresh(s_led);
}

static esp_err_t rgb_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = RGB_GPIO,
        .max_leds = RGB_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };
    return led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_led);
}

static void startup_blink_white(int count)
{
    for (int i = 0; i < count; i++) {
        rgb_set(40, 40, 40);
        vTaskDelay(pdMS_TO_TICKS(100));
        rgb_set(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static esp_err_t uac_device_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *arg)
{
    (void)arg;
    int16_t *samples = (int16_t *)buf;
    size_t n = len / sizeof(int16_t);

    static int sample_count = 0;
    static int16_t current_val = 5000;

    for (size_t i = 0; i < n; i++) {
        samples[i] = current_val;
        if (++sample_count >= 50) {
            current_val = -current_val;
            sample_count = 0;
        }
    }

    *bytes_read = len;
    return ESP_OK;
}

static void uac_device_set_mute_cb(uint32_t mute, void *arg)
{
    (void)arg;
    /* mute=0 => unmuted; host often opens capture with stream unmuted */
    s_stream_active = (mute == 0);
    ESP_LOGI(TAG, "mute callback: %" PRIu32 " (stream_active=%d)", mute, (int)s_stream_active);
    if (s_stream_active) {
        rgb_set(0, 255, 0);
    }
}

static void led_task(void *arg)
{
    (void)arg;
    bool pulse_on = false;

    for (;;) {
        if (!s_stream_active) {
            pulse_on = !pulse_on;
            rgb_set(0, 0, pulse_on ? 255 : 0);
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Node_A_USB_Debug_v3_idf starting");

    if (rgb_init() != ESP_OK) {
        ESP_LOGE(TAG, "RGB init failed (GPIO %d)", RGB_GPIO);
    } else {
        rgb_set(0, 0, 0);
        startup_blink_white(5);
    }

    uac_device_config_t config = {
        .skip_tinyusb_init = false,
        .output_cb = NULL,
        .input_cb = uac_device_input_cb,
        .set_mute_cb = uac_device_set_mute_cb,
        .set_volume_cb = NULL,
        .cb_ctx = NULL,
    };

    esp_err_t err = uac_device_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_device_init failed: %s", esp_err_to_name(err));
        rgb_set(255, 0, 0);
        return;
    }

    ESP_LOGI(TAG, "UAC device ready (48 kHz mono mic)");

    xTaskCreatePinnedToCore(led_task, "led", 4096, NULL, 5, NULL, 1);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
