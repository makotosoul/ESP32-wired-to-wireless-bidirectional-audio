/*
 * Node_A_Audio_idf
 *
 * USB UAC speaker (44.1 kHz STEREO) that forwards host audio over ESP-NOW
 * to Node_B_Audio.ino on the A1S Audio Kit. Pure speaker direction only;
 * mic interface is disabled.
 *
 * Why this project exists:
 *   Isolation test for the patched usb_device_uac speaker OUT path in an
 *   IDF/PlatformIO build, paired with the known-good Arduino Node_B_Audio.
 *   Same wire format as the old Arduino Node_A_Audio.ino, so Node B is
 *   re-used unchanged. If audio plays end-to-end here but Node_A_Bridge_idf
 *   stays silent on the speaker direction, the bridge bug is in the
 *   combined mic+speaker configuration, not the speaker path itself.
 *
 * Wire format (matches Node_A_Audio.ino + Node_B_Audio.ino):
 *   Packet:    240 bytes per ESP-NOW transmission
 *   Encoding:  IMA ADPCM 4-bit, codeL in low nibble, codeR in high nibble
 *   Decoded:   240 stereo pairs = 480 int16 samples ≈ 5.44 ms @ 44.1 kHz
 *
 * UAC OUT pull rate (host -> us):
 *   CONFIG_UAC_SPK_INTERVAL_MS=10 => uac_output_cb receives 1764-byte
 *   buffers (882 stereo samples / call). We accumulate into a 480-sample
 *   buffer and emit one ESP-NOW packet whenever it fills (~5.44 ms cadence).
 *
 * LED diagnostic (so we can debug without serial — the S3's USB-OTG owns
 * the only USB port once UAC is up, hiding /dev/ttyACM0):
 *   Blue blink slow  -> idle, no UAC bytes from host
 *   Green solid      -> uac_output_cb firing (host IS delivering bytes)
 *   Magenta solid    -> ESP-NOW sends acknowledged recently
 *   White solid      -> end-to-end OK (the desired steady state)
 *   Red blink fast   -> ESP-NOW send failures in the last LIVE_MS window
 */

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "nvs_flash.h"

#include "led_strip.h"
#include "usb_device_uac.h"

static const char *TAG = "audio_idf";

/* Node B MAC — mirrors peers.h (regenerate via auto_mac_setup.sh if boards swap). */
static const uint8_t NODE_B_ADDR[6] = {0xe0, 0x8c, 0xfe, 0x63, 0xff, 0x28};

#define ESP_NOW_CHANNEL     1

/* 240 ADPCM bytes -> 240 stereo pairs -> 480 int16 samples -> 960 raw PCM bytes. */
#define PACKET_SIZE         240
#define RAW_SAMPLES         480

/* LED */
#define RGB_GPIO            48
#define RGB_LED_COUNT       1
#define LIVE_MS             200

static led_strip_handle_t   s_led;

static volatile uint32_t    s_last_uac_ms;
static volatile uint32_t    s_last_now_ok_ms;
static volatile uint32_t    s_last_now_fail_ms;

static volatile uint32_t    s_uac_bytes;
static volatile uint32_t    s_now_ok;
static volatile uint32_t    s_now_fail;

/* Sample accumulator: encode/emit a packet every RAW_SAMPLES samples received. */
static int16_t              s_acc_buf[RAW_SAMPLES];
static size_t               s_acc_idx;

/* ===================== IMA ADPCM encoder =====================
 * Tables and encode function are bit-for-bit identical to Node_A_Audio.ino
 * (lines 18-48) so the encoder state machine and the resulting nibble
 * codes match exactly what Node_B_Audio.ino expects.
 */
static const int s_indexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

static const int s_stepsizeTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

typedef struct {
    int32_t predictor;
    int8_t  index;
} adpcm_state_t;

static adpcm_state_t s_enc_L = {0, 0};
static adpcm_state_t s_enc_R = {0, 0};

static inline uint8_t adpcm_encode(int16_t sample, adpcm_state_t *st)
{
    int32_t diff  = sample - st->predictor;
    uint8_t code  = 0;
    int32_t step  = s_stepsizeTable[st->index];
    int32_t diffq = step >> 3;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    if (diff >= step) {
        code |= 4;
        diff -= step;
        diffq += step;
    }
    step >>= 1;
    if (diff >= step) {
        code |= 2;
        diff -= step;
        diffq += step;
    }
    step >>= 1;
    if (diff >= step) {
        code |= 1;
        diffq += step;
    }
    if (code & 8) st->predictor -= diffq;
    else          st->predictor += diffq;
    if (st->predictor >  32767) st->predictor =  32767;
    if (st->predictor < -32768) st->predictor = -32768;
    st->index += s_indexTable[code & 0x07];
    if (st->index <  0) st->index = 0;
    if (st->index > 88) st->index = 88;
    return code;
}

static void encode_and_send_packet(void)
{
    uint8_t pkt[PACKET_SIZE];
    for (int j = 0; j < RAW_SAMPLES; j += 2) {
        uint8_t codeL = adpcm_encode(s_acc_buf[j],     &s_enc_L);
        uint8_t codeR = adpcm_encode(s_acc_buf[j + 1], &s_enc_R);
        /* Wire layout matches Node_A_Audio.ino: codeL in low nibble, codeR high. */
        pkt[j / 2] = (uint8_t)((codeR << 4) | (codeL & 0x0F));
    }
    esp_err_t err = esp_now_send(NODE_B_ADDR, pkt, sizeof(pkt));
    if (err != ESP_OK) {
        /* Synchronous queue failure (e.g. WiFi tx ring full). The async
         * on_now_send() callback only reports per-packet ACK status; this
         * path catches the "couldn't even enqueue" cases. */
        s_now_fail++;
        s_last_now_fail_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    }
}

/* ===================== UAC speaker OUT callback =====================
 * Runs from usb_spk_task (pinned to Core 1 via CONFIG_UAC_SPK_TASK_CORE=1).
 * We always copy len bytes — the host's clock advances regardless of what
 * we do, so we can't stall here. esp_now_send() is non-blocking (queues
 * into the WiFi tx ring) so calling it inline is safe in this context.
 */
static esp_err_t uac_output_cb(uint8_t *buf, size_t len, void *arg)
{
    (void)arg;

    s_uac_bytes  += len;
    s_last_uac_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    const int16_t *samples = (const int16_t *)buf;
    size_t count = len / sizeof(int16_t);

    for (size_t i = 0; i < count; i++) {
        s_acc_buf[s_acc_idx++] = samples[i];
        if (s_acc_idx >= RAW_SAMPLES) {
            encode_and_send_packet();
            s_acc_idx = 0;
        }
    }
    return ESP_OK;
}

/* ===================== ESP-NOW send completion callback ===================== */
static void on_now_send(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (status == ESP_NOW_SEND_SUCCESS) {
        s_now_ok++;
        s_last_now_ok_ms = now;
    } else {
        s_now_fail++;
        s_last_now_fail_ms = now;
    }
}

/* Logged via ESP_LOGI (not visible without serial console) but kept because
 * registering a mute callback also encourages the host to send unmute on
 * stream open, which can help PipeWire actually start delivering data. */
static void uac_mute_cb(uint32_t mute, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "host mute=%" PRIu32, mute);
}

/* ===================== LED ===================== */
static void rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_led == NULL) return;
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

static void led_task(void *arg)
{
    (void)arg;
    bool pulse_on = false;

    for (;;) {
        uint32_t now       = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        bool uac_live      = (now - s_last_uac_ms)      < LIVE_MS;
        bool now_ok_live   = (now - s_last_now_ok_ms)   < LIVE_MS;
        bool now_fail_live = (now - s_last_now_fail_ms) < LIVE_MS;

        if (now_fail_live) {
            /* Highest priority: surface ESP-NOW failures even if other paths look fine. */
            pulse_on = !pulse_on;
            rgb_set(pulse_on ? 255 : 0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else if (uac_live && now_ok_live) {
            rgb_set(255, 255, 255);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else if (now_ok_live) {
            rgb_set(255, 0, 255);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else if (uac_live) {
            rgb_set(0, 255, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            pulse_on = !pulse_on;
            rgb_set(0, 0, pulse_on ? 255 : 0);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

static void stats_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP_LOGI(TAG,
                 "uac=%" PRIu32 "B now_ok=%" PRIu32 " now_fail=%" PRIu32,
                 s_uac_bytes, s_now_ok, s_now_fail);
        s_uac_bytes = 0;
        s_now_ok    = 0;
        s_now_fail  = 0;
    }
}

/* ===================== WiFi + ESP-NOW init ===================== */
static void wifi_espnow_init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESP_NOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "STA MAC %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_now_send));

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, NODE_B_ADDR, 6);
    peer.channel = ESP_NOW_CHANNEL;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

/* ===================== app_main ===================== */
void app_main(void)
{
    ESP_LOGI(TAG, "Node_A_Audio_idf (44.1 kHz stereo UAC speaker, ESP-NOW ch %d)",
             ESP_NOW_CHANNEL);

    if (rgb_init() != ESP_OK) {
        ESP_LOGE(TAG, "RGB init failed (GPIO %d)", RGB_GPIO);
    } else {
        rgb_set(0, 0, 0);
        startup_blink_white(5);
    }

    wifi_espnow_init();

    uac_device_config_t cfg = {
        .skip_tinyusb_init = false,
        .output_cb     = uac_output_cb,
        .input_cb      = NULL,
        .set_mute_cb   = uac_mute_cb,
        .set_volume_cb = NULL,
        .cb_ctx        = NULL,
    };
    esp_err_t err = uac_device_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_device_init failed: %s", esp_err_to_name(err));
        rgb_set(255, 0, 0);
        return;
    }

    ESP_LOGI(TAG, "UAC ready (speaker @ 44.1 kHz stereo)");

    xTaskCreatePinnedToCore(led_task,   "led",   4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(stats_task, "stats", 4096, NULL, 3, NULL, 1);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
