/*
 * Node_A_Bridge_idf
 *
 * Bidirectional USB UAC bridge (48 kHz mono mic + speaker) over ESP-NOW.
 *
 * Host speaker OUT -> uac_output_cb -> StreamBuffer -> spk_tx_task -> ESP-NOW
 *   Opus v2 / 10 ms (480 mono samples @ 48 kHz), <= 250 B variable length.
 *
 * ESP-NOW mic <- Node B: 200 B ADPCM / 25 ms -> decode 16 kHz mono ->
 *   upsample 3x linear -> StreamBuffer -> uac_input_cb -> host mic IN
 *   960 B / 10 ms (480 samples @ 48 kHz).
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
#include "opus.h"
#include "bridge_spk.h"
#include "peers.h"

static const char *TAG = "bridge_a";

/* Node B MAC — from peers.h (update via auto_mac_setup.sh if boards change). */
static const uint8_t NODE_B_ADDR[6] = {PEER_MAC_B};

#define ESP_NOW_CHANNEL         1

/* Mic wireless: 16 kHz mono ADPCM from Node B */
#define MIC_PKT_BYTES           200
#define MIC_SAMPLES_16K         400
#define MIC_UPSAMPLED_SAMPLES   (MIC_SAMPLES_16K * 3)
#define MIC_UPSAMPLED_BYTES     (MIC_UPSAMPLED_SAMPLES * (int)sizeof(int16_t))

/* Speaker wireless: 48 kHz mono Opus to Node B (see bridge_spk.h). */
#define SPK_SAMPLES_48K         BRIDGE_SPK_FRAME_SAMPLES
#define SPK_PCM_BYTES           BRIDGE_SPK_PCM_BYTES

/* UAC @ 48 kHz mono, 10 ms interval */
#define UAC_FRAME_BYTES         960

#define STREAM_BUF_MIC_BYTES    (MIC_UPSAMPLED_BYTES * 6)
#define STREAM_BUF_SPK_BYTES    (SPK_PCM_BYTES * 12)
#define STREAM_TRIGGER          1
#define SPK_STREAM_MAX_FRAMES   8

#define RGB_GPIO                48
#define RGB_LED_COUNT           1
#define TRAFFIC_LIVE_MS         200

static led_strip_handle_t   s_led;
static StreamBufferHandle_t s_mic_stream;
static StreamBufferHandle_t s_spk_stream;

static volatile uint32_t    s_last_mic_pkt_ms;
static volatile uint32_t    s_last_spk_tx_ms;
/* LED uses USB callback times (same as bisection tests). Wireless mic from Node B
 * always updates s_last_mic_pkt_ms but must NOT turn the LED green at idle. */
static volatile uint32_t    s_last_uac_in_ms;
static volatile uint32_t    s_last_uac_out_ms;

static volatile uint32_t    s_rx_mic_pkts;
static volatile uint32_t    s_rx_bad_len;
static volatile uint32_t    s_mic_overflow_bytes;
static volatile uint32_t    s_mic_underrun_bytes;
static volatile uint32_t    s_spk_overflow_bytes;
static volatile uint32_t    s_spk_uac_bytes;
static volatile uint32_t    s_spk_now_fail;
static volatile uint32_t    s_spk_now_ok;

static int16_t              s_upsample_prev;

/* ===================== IMA ADPCM ===================== */
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

static adpcm_state_t s_dec_mic = {0, 0};

static OpusEncoder *s_opus_enc;

/* Large PCM scratch — keep off task stacks (especially usb_spk_task). */
static int16_t s_mic_pcm16[MIC_SAMPLES_16K];
static int16_t s_mic_pcm48[MIC_UPSAMPLED_SAMPLES];
static int16_t s_spk_trash[SPK_SAMPLES_48K];

static inline int16_t adpcm_decode(uint8_t code, adpcm_state_t *st)
{
    int32_t step  = s_stepsizeTable[st->index];
    int32_t diffq = step >> 3;
    if (code & 4) diffq += step;
    if (code & 2) diffq += (step >> 1);
    if (code & 1) diffq += (step >> 2);
    if (code & 8) st->predictor -= diffq;
    else st->predictor += diffq;
    if (st->predictor >  32767) st->predictor =  32767;
    if (st->predictor < -32768) st->predictor = -32768;
    st->index += s_indexTable[code & 0x07];
    if (st->index <  0) st->index = 0;
    if (st->index > 88) st->index = 88;
    return (int16_t)st->predictor;
}

static inline uint8_t adpcm_encode(int16_t sample, adpcm_state_t *st)
{
    int32_t diff = sample - st->predictor;
    uint8_t code = 0;
    int32_t step = s_stepsizeTable[st->index];
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
    else st->predictor += diffq;
    if (st->predictor >  32767) st->predictor =  32767;
    if (st->predictor < -32768) st->predictor = -32768;
    st->index += s_indexTable[code & 0x07];
    if (st->index <  0) st->index = 0;
    if (st->index > 88) st->index = 88;
    return code;
}

/* 16 kHz mono (n_in samples) -> 48 kHz mono (3 * n_in samples). */
static void upsample_3x_linear(const int16_t *in, size_t n_in, int16_t *out)
{
    int16_t s0 = s_upsample_prev;
    int16_t s1 = in[0];
    out[0] = s0;
    out[1] = (int16_t)(((int32_t)2 * s0 + s1) / 3);
    out[2] = (int16_t)(((int32_t)s0 + 2 * s1) / 3);

    size_t o = 3;
    for (size_t i = 0; i + 1 < n_in; i++) {
        s0 = in[i];
        s1 = in[i + 1];
        out[o++] = s0;
        out[o++] = (int16_t)(((int32_t)2 * s0 + s1) / 3);
        out[o++] = (int16_t)(((int32_t)s0 + 2 * s1) / 3);
    }
    s_upsample_prev = in[n_in - 1];
}

/* ===================== ESP-NOW ===================== */
static void on_now_send(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    if (status == ESP_NOW_SEND_SUCCESS) {
        s_spk_now_ok++;
        s_last_spk_tx_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    } else {
        s_spk_now_fail++;
    }
}

static void on_now_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    (void)info;

    if (len != MIC_PKT_BYTES) {
        s_rx_bad_len++;
        return;
    }

    for (int i = 0; i < MIC_PKT_BYTES; i++) {
        uint8_t low  = data[i] & 0x0F;
        uint8_t high = (data[i] >> 4) & 0x0F;
        s_mic_pcm16[i * 2]     = adpcm_decode(low,  &s_dec_mic);
        s_mic_pcm16[i * 2 + 1] = adpcm_decode(high, &s_dec_mic);
    }

    upsample_3x_linear(s_mic_pcm16, MIC_SAMPLES_16K, s_mic_pcm48);

    size_t sent = xStreamBufferSend(s_mic_stream, s_mic_pcm48, sizeof(s_mic_pcm48), 0);
    if (sent < sizeof(s_mic_pcm48)) {
        s_mic_overflow_bytes += (sizeof(s_mic_pcm48) - sent);
    }
    s_rx_mic_pkts++;
    s_last_mic_pkt_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* Only call from spk_tx_task (reader). Never from uac_output_cb (writer). */
static void spk_discard_backlog_frames(void)
{
    while (xStreamBufferBytesAvailable(s_spk_stream) > SPK_PCM_BYTES * SPK_STREAM_MAX_FRAMES) {
        if (xStreamBufferReceive(s_spk_stream, s_spk_trash, sizeof(s_spk_trash), 0) != sizeof(s_spk_trash)) {
            break;
        }
    }
}

static esp_err_t spk_opus_encoder_init(void)
{
    int err = OPUS_OK;
    s_opus_enc = opus_encoder_create(BRIDGE_SPK_SAMPLE_RATE_HZ, 1,
                                     OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK || s_opus_enc == NULL) {
        ESP_LOGE(TAG, "opus_encoder_create failed: %s", opus_strerror(err));
        return ESP_FAIL;
    }
    opus_encoder_ctl(s_opus_enc, OPUS_SET_BITRATE(BRIDGE_SPK_OPUS_BITRATE));
    opus_encoder_ctl(s_opus_enc, OPUS_SET_COMPLEXITY(BRIDGE_SPK_OPUS_COMPLEXITY_ENC));
    opus_encoder_ctl(s_opus_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(s_opus_enc, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
    opus_encoder_ctl(s_opus_enc, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
    ESP_LOGI(TAG, "Opus speaker encoder: %d Hz mono, %d b/s, complexity %d",
             BRIDGE_SPK_SAMPLE_RATE_HZ, BRIDGE_SPK_OPUS_BITRATE,
             BRIDGE_SPK_OPUS_COMPLEXITY_ENC);
    return ESP_OK;
}

static esp_err_t spk_opus_warmup_encode(void)
{
    static int16_t silence[SPK_SAMPLES_48K];
    static uint8_t pkt[BRIDGE_SPK_MAX_PKT_BYTES];

    memset(silence, 0, sizeof(silence));
    pkt[0] = BRIDGE_SPK_OPUS_MAGIC;
    int opus_bytes = opus_encode(s_opus_enc, silence, SPK_SAMPLES_48K,
                                 &pkt[BRIDGE_SPK_OPUS_HDR_BYTES],
                                 BRIDGE_SPK_MAX_PKT_BYTES - BRIDGE_SPK_OPUS_HDR_BYTES);
    if (opus_bytes < 0) {
        ESP_LOGE(TAG, "opus warmup encode failed: %s", opus_strerror(opus_bytes));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Opus warmup OK (%d B frame)", opus_bytes);
    return ESP_OK;
}

static void spk_tx_task(void *arg)
{
    (void)arg;
    static int16_t pcm[SPK_SAMPLES_48K];
    static uint8_t pkt[BRIDGE_SPK_MAX_PKT_BYTES];

    for (;;) {
        spk_discard_backlog_frames();

        /* usb_device_uac delivers ~96 B per read; do not xStreamBufferReceive
         * with a timeout — on expiry FreeRTOS returns a partial chunk and removes
         * it from the buffer, desynchronizing 960-byte (10 ms) frames. */
        if (xStreamBufferBytesAvailable(s_spk_stream) < sizeof(pcm)) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        size_t got = xStreamBufferReceive(s_spk_stream, pcm, sizeof(pcm), 0);
        if (got != sizeof(pcm)) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        pkt[0] = BRIDGE_SPK_OPUS_MAGIC;
        int opus_bytes = opus_encode(s_opus_enc, pcm, SPK_SAMPLES_48K,
                                     &pkt[BRIDGE_SPK_OPUS_HDR_BYTES],
                                     BRIDGE_SPK_MAX_PKT_BYTES - BRIDGE_SPK_OPUS_HDR_BYTES);
        if (opus_bytes < 0) {
            ESP_LOGW(TAG, "opus_encode: %s", opus_strerror(opus_bytes));
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        pkt[1] = (uint8_t)opus_bytes;
        int pkt_len = BRIDGE_SPK_OPUS_HDR_BYTES + opus_bytes;

        esp_err_t err = esp_now_send(NODE_B_ADDR, pkt, (size_t)pkt_len);
        if (err != ESP_OK) {
            s_spk_now_fail++;
        } else {
            s_last_spk_tx_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        }

        /* Yield so IDLE/WDT stay healthy when USB backlog sends many frames. */
        vTaskDelay(1);
    }
}

/* ===================== UAC callbacks ===================== */
static esp_err_t uac_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *arg)
{
    (void)arg;

    s_last_uac_in_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    size_t got = xStreamBufferReceive(s_mic_stream, buf, len, 0);
    if (got < len) {
        memset(buf + got, 0, len - got);
        s_mic_underrun_bytes += (len - got);
    }
    *bytes_read = len;
    return ESP_OK;
}

static esp_err_t uac_output_cb(uint8_t *buf, size_t len, void *arg)
{
    (void)arg;

    s_last_uac_out_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_spk_uac_bytes += len;
    size_t sent = xStreamBufferSend(s_spk_stream, buf, len, 0);
    if (sent < len) {
        s_spk_overflow_bytes += (len - sent);
    }
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

static void led_task(void *arg)
{
    (void)arg;
    bool pulse_on = false;

    for (;;) {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        bool usb_mic = (now_ms - s_last_uac_in_ms) < TRAFFIC_LIVE_MS;
        bool usb_spk = (now_ms - s_last_uac_out_ms) < TRAFFIC_LIVE_MS;

        if (usb_mic && usb_spk) {
            rgb_set(255, 255, 255);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else if (usb_spk) {
            rgb_set(255, 0, 255);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else if (usb_mic) {
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
                 "mic_rx=%" PRIu32 " bad=%" PRIu32
                 " mic_ov=%" PRIu32 "B mic_un=%" PRIu32 "B"
                 " spk_uac=%" PRIu32 "B spk_ov=%" PRIu32 "B now_ok=%" PRIu32 " now_fail=%" PRIu32,
                 s_rx_mic_pkts, s_rx_bad_len,
                 s_mic_overflow_bytes, s_mic_underrun_bytes,
                 s_spk_uac_bytes, s_spk_overflow_bytes, s_spk_now_ok, s_spk_now_fail);
        s_rx_mic_pkts = 0;
        s_rx_bad_len = 0;
        s_mic_overflow_bytes = 0;
        s_mic_underrun_bytes = 0;
        s_spk_uac_bytes = 0;
        s_spk_overflow_bytes = 0;
        s_spk_now_ok = 0;
        s_spk_now_fail = 0;
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
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_now_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_now_send));

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, NODE_B_ADDR, 6);
    peer.channel = ESP_NOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Node_A_Bridge_idf (48 kHz UAC mono mic+spk, Opus speaker, ESP-NOW ch %d)",
             ESP_NOW_CHANNEL);

    if (rgb_init() != ESP_OK) {
        ESP_LOGE(TAG, "RGB init failed (GPIO %d)", RGB_GPIO);
    } else {
        rgb_set(0, 0, 0);
        startup_blink_white(5);
    }

    s_mic_stream = xStreamBufferCreate(STREAM_BUF_MIC_BYTES, STREAM_TRIGGER);
    s_spk_stream = xStreamBufferCreate(STREAM_BUF_SPK_BYTES, STREAM_TRIGGER);
    if (s_mic_stream == NULL || s_spk_stream == NULL) {
        ESP_LOGE(TAG, "stream buffer alloc failed");
        rgb_set(255, 0, 0);
        return;
    }

    wifi_espnow_init();

    if (spk_opus_encoder_init() != ESP_OK || spk_opus_warmup_encode() != ESP_OK) {
        ESP_LOGE(TAG, "Opus speaker encoder init failed — fix SPIRAM / heap");
        rgb_set(255, 0, 0);
        return;
    }

    uac_device_config_t cfg = {
        .skip_tinyusb_init = false,
        .output_cb = uac_output_cb,
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

    ESP_LOGI(TAG, "UAC ready (mic+speaker @ 48 kHz mono)");

    {
        uint32_t t0 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        s_last_uac_in_ms  = t0 - TRAFFIC_LIVE_MS - 1;
        s_last_uac_out_ms = t0 - TRAFFIC_LIVE_MS - 1;
        s_last_spk_tx_ms  = t0 - TRAFFIC_LIVE_MS - 1;
        s_last_mic_pkt_ms = t0 - TRAFFIC_LIVE_MS - 1;
    }

    xTaskCreatePinnedToCore(spk_tx_task, "spk_tx", 16384, NULL, 8, NULL, 0);
    xTaskCreatePinnedToCore(led_task,   "led",   4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(stats_task, "stats", 4096, NULL, 3, NULL, 1);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
