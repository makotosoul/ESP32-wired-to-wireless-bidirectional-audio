/*
 * Node_A_Mic_Test_v2_idf
 *
 * USB UAC microphone (16 kHz mono, 16-bit) that streams audio received over
 * ESP-NOW from Node B (Node_B_Mic_Test.ino, A1S Audio Kit). Replaces the
 * synthetic tone of Node_A_USB_Debug_v3_idf with real PCM.
 *
 * Wire format (per ESP-NOW packet from Node B):
 *   200 bytes, IMA ADPCM 4-bit, 2 samples per byte
 *   => 400 int16_t PCM samples after decode = 800 bytes = 25 ms @ 16 kHz mono.
 *
 * UAC pull rate (host -> us):
 *   CONFIG_UAC_MIC_INTERVAL_MS=10, so uac_input_cb receives a 320-byte buffer
 *   every ~10 ms (160 samples).
 *
 * Producer/consumer mismatch (25 ms packets vs 10 ms pulls) is absorbed by a
 * StreamBuffer. On underrun we zero-fill so the USB isochronous clock keeps
 * advancing (otherwise the host sees stalls instead of silence).
 */

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

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

static const char *TAG = "uac_mic_v2";

/* ---------- Peer MAC (Node B / A1S). Mirrors peers.h. ---------- */
/* Node B MAC discovered by auto_mac_setup.sh on /dev/ttyUSB0. */
static const uint8_t NODE_B_ADDR[6] = {0xe0, 0x8c, 0xfe, 0x63, 0xff, 0x28};

/* ---------- Wire format constants ---------- */
#define ESP_NOW_CHANNEL     1
#define PACKET_SIZE         200          /* ADPCM bytes per packet */
#define SAMPLES_PER_PACKET  (PACKET_SIZE * 2) /* 400 int16_t after decode */
#define PCM_BYTES_PER_PKT   (SAMPLES_PER_PACKET * (int)sizeof(int16_t)) /* 800 */

/* ---------- StreamBuffer sizing ---------- */
/* Hold ~6 packets of PCM = 4800 bytes ≈ 150 ms of audio jitter slack. */
#define STREAM_BUF_BYTES    (PCM_BYTES_PER_PKT * 6)
#define STREAM_TRIGGER      1

/* ---------- LED ---------- */
#define RGB_GPIO            48
#define RGB_LED_COUNT       1
/* "Recently received a packet" window for the green LED state. */
#define STREAM_LIVE_MS      200

/* ---------- Globals ---------- */
static led_strip_handle_t   s_led;
static StreamBufferHandle_t s_pcm_stream;
static volatile uint32_t    s_last_pkt_ms;
static volatile uint32_t    s_rx_packets;
static volatile uint32_t    s_rx_bad_len;
static volatile uint32_t    s_overflow_bytes;
static volatile uint32_t    s_underrun_bytes;

/* ===================== IMA ADPCM decoder =====================
 * Tables and decode function copied verbatim from
 * Node_A_Mic_Test/Node_A_Mic_Test.ino (lines 22-47) so the wire format
 * stays identical to the existing Arduino transmitter on Node B.
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

static adpcm_state_t s_dec_state = {0, 0};

static inline int16_t adpcm_decode(uint8_t code, adpcm_state_t *st)
{
    int32_t step  = s_stepsizeTable[st->index];
    int32_t diffq = step >> 3;
    if (code & 4) diffq += step;
    if (code & 2) diffq += (step >> 1);
    if (code & 1) diffq += (step >> 2);
    if (code & 8) st->predictor -= diffq; else st->predictor += diffq;
    if (st->predictor >  32767) st->predictor =  32767;
    if (st->predictor < -32768) st->predictor = -32768;
    st->index += s_indexTable[code & 0x07];
    if (st->index <  0) st->index = 0;
    if (st->index > 88) st->index = 88;
    return (int16_t)st->predictor;
}

/* ===================== ESP-NOW receive ===================== */
static void on_now_recv(const esp_now_recv_info_t *info,
                        const uint8_t *data, int len)
{
    (void)info;

    if (len != PACKET_SIZE) {
        s_rx_bad_len++;
        return;
    }

    int16_t pcm[SAMPLES_PER_PACKET];
    for (int i = 0; i < PACKET_SIZE; i++) {
        uint8_t low  = data[i] & 0x0F;
        uint8_t high = (data[i] >> 4) & 0x0F;
        pcm[i * 2]     = adpcm_decode(low,  &s_dec_state);
        pcm[i * 2 + 1] = adpcm_decode(high, &s_dec_state);
    }

    /* The ESP-NOW recv callback runs from the WiFi task (not an ISR), so the
     * non-ISR FreeRTOS APIs are correct here. Drop on overflow rather than
     * block, otherwise we'd stall WiFi reception just to wait for the UAC
     * consumer to drain. A 25 ms audio gap is preferable. */
    size_t sent = xStreamBufferSend(s_pcm_stream, pcm, sizeof(pcm), 0);
    if (sent < sizeof(pcm)) {
        s_overflow_bytes += (sizeof(pcm) - sent);
    }
    s_rx_packets++;
    s_last_pkt_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ===================== UAC input callback =====================
 * Called by the TinyUSB / usb_device_uac task every CONFIG_UAC_MIC_INTERVAL_MS.
 * `len` is fixed per call (320 bytes for our config). We always return
 * *bytes_read = len; missing bytes are filled with silence so the
 * isochronous IN endpoint never stalls.
 */
static esp_err_t uac_input_cb(uint8_t *buf, size_t len,
                              size_t *bytes_read, void *arg)
{
    (void)arg;

    size_t got = xStreamBufferReceive(s_pcm_stream, buf, len,
                                      pdMS_TO_TICKS(5));
    if (got < len) {
        memset(buf + got, 0, len - got);
        s_underrun_bytes += (len - got);
    }
    *bytes_read = len;
    return ESP_OK;
}

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
    bool streaming_last = false;

    for (;;) {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        bool streaming = (now_ms - s_last_pkt_ms) < STREAM_LIVE_MS;

        if (streaming) {
            if (!streaming_last) {
                rgb_set(0, 255, 0);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            pulse_on = !pulse_on;
            rgb_set(0, 0, pulse_on ? 255 : 0);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        streaming_last = streaming;
    }
}

static void stats_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP_LOGI(TAG,
                 "rx=%" PRIu32 " bad_len=%" PRIu32
                 " overflow=%" PRIu32 "B underrun=%" PRIu32 "B",
                 s_rx_packets, s_rx_bad_len,
                 s_overflow_bytes, s_underrun_bytes);
        s_rx_packets = 0;
        s_rx_bad_len = 0;
        s_overflow_bytes = 0;
        s_underrun_bytes = 0;
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
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESP_NOW_CHANNEL,
                                         WIFI_SECOND_CHAN_NONE));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "STA MAC %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_now_recv));

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, NODE_B_ADDR, 6);
    peer.channel = ESP_NOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

/* ===================== app_main ===================== */
void app_main(void)
{
    ESP_LOGI(TAG, "Node_A_Mic_Test_v2_idf starting "
                  "(16 kHz mono, ESP-NOW ch %d)", ESP_NOW_CHANNEL);

    if (rgb_init() != ESP_OK) {
        ESP_LOGE(TAG, "RGB init failed (GPIO %d)", RGB_GPIO);
    } else {
        rgb_set(0, 0, 0);
        startup_blink_white(5);
    }

    s_pcm_stream = xStreamBufferCreate(STREAM_BUF_BYTES, STREAM_TRIGGER);
    if (s_pcm_stream == NULL) {
        ESP_LOGE(TAG, "stream buffer alloc failed");
        rgb_set(255, 0, 0);
        return;
    }

    wifi_espnow_init();

    uac_device_config_t cfg = {
        .skip_tinyusb_init = false,
        .output_cb = NULL,
        .input_cb = uac_input_cb,
        .set_mute_cb = uac_mute_cb,
        .set_volume_cb = NULL,
        .cb_ctx = NULL,
    };
    esp_err_t err = uac_device_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_device_init failed: %s", esp_err_to_name(err));
        rgb_set(255, 0, 0);
        return;
    }

    ESP_LOGI(TAG, "UAC device ready");

    xTaskCreatePinnedToCore(led_task,   "led",   4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(stats_task, "stats", 4096, NULL, 3, NULL, 1);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
