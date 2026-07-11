/*
 * Node_A_Spk_Mic_Test_idf
 *
 * Bisection Test C for the Node_A_Bridge_idf speaker bug.
 *
 * Configuration:
 *   - Speaker: 44.1 kHz STEREO  (identical to working Node_A_Audio_idf)
 *   - Mic:     44.1 kHz MONO   (NEW vs audio_idf -- silent output)
 *   => CFG_TUD_AUDIO_FUNC_1_N_AS_INT resolves to 2 (the patched codepath)
 *
 * Pairs with the unmodified Node_B_Audio.ino. Wire format for the
 * speaker direction is byte-for-byte identical to audio_idf:
 *   240 B IMA ADPCM packet = 240 stereo pairs = 480 int16 samples
 *   (codeL in low nibble, codeR in high nibble).
 *
 * Diagnostic LED (since /dev/ttyACM0 is hidden by UAC):
 *   Blue blink slow  -> idle, no UAC callbacks firing
 *   Green solid      -> only mic input_cb firing (host pulling silence)
 *   Magenta solid    -> only speaker uac_output_cb firing
 *   White solid      -> both firing (the expected steady state)
 *   Red blink fast   -> ESP-NOW send failures
 *
 * Interpretation of the test:
 *   - WHITE (and audio plays on Node B): N_AS_INT=2 path works fine.
 *     Bridge bug is in {48 kHz, mono spk, or both with mic}. Proceed
 *     to Test B (48 kHz stereo) and Test A (48 kHz mono).
 *   - GREEN only (audio does NOT play): reproduced the bridge bug here.
 *     Mic interface is alive but speaker callback never fires. The
 *     patched usb_device_uac N_AS_INT=2 path is broken. Fix is in the
 *     vendored component (e.g. another macro that needs to scale with
 *     N_AS_INT, or a TinyUSB version mismatch).
 *   - BLUE only: host never opens streams. PipeWire / descriptor issue.
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

static const char *TAG = "spk_mic_test";

/* Node B MAC -- mirrors peers.h. */
static const uint8_t NODE_B_ADDR[6] = {0xe0, 0x8c, 0xfe, 0x63, 0xff, 0x28};

#define ESP_NOW_CHANNEL     1

/* Speaker wire format (identical to Node_A_Audio.ino + Node_A_Audio_idf). */
#define PACKET_SIZE         240
#define RAW_SAMPLES         480

/* LED */
#define RGB_GPIO            48
#define RGB_LED_COUNT       1
#define LIVE_MS             200

static led_strip_handle_t   s_led;

static volatile uint32_t    s_last_uac_out_ms;   /* speaker callback fires */
static volatile uint32_t    s_last_uac_in_ms;    /* mic callback fires */
static volatile uint32_t    s_last_now_ok_ms;
static volatile uint32_t    s_last_now_fail_ms;

static volatile uint32_t    s_uac_out_bytes;
static volatile uint32_t    s_uac_in_bytes;
static volatile uint32_t    s_now_ok;
static volatile uint32_t    s_now_fail;

static int16_t              s_acc_buf[RAW_SAMPLES];
static size_t               s_acc_idx;

/* ===================== IMA ADPCM encoder (matches audio_idf / Node_B_Audio) */
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
        pkt[j / 2] = (uint8_t)((codeR << 4) | (codeL & 0x0F));
    }
    esp_err_t err = esp_now_send(NODE_B_ADDR, pkt, sizeof(pkt));
    if (err != ESP_OK) {
        s_now_fail++;
        s_last_now_fail_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    }
}

/* ===================== UAC speaker OUT callback ===================== */
static esp_err_t uac_output_cb(uint8_t *buf, size_t len, void *arg)
{
    (void)arg;
    s_uac_out_bytes += len;
    s_last_uac_out_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

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

/* ===================== UAC mic IN callback (silent) =====================
 * The whole point of this test: have the mic interface present and being
 * pulled by the host without doing anything interesting. Just fill the
 * buffer with silence so the isochronous IN endpoint never stalls.
 */
static esp_err_t uac_input_cb(uint8_t *buf, size_t len,
                              size_t *bytes_read, void *arg)
{
    (void)arg;
    memset(buf, 0, len);
    *bytes_read = len;
    s_uac_in_bytes += len;
    s_last_uac_in_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    return ESP_OK;
}

/* ===================== ESP-NOW send callback ===================== */
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
        uint32_t now      = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        bool spk_live     = (now - s_last_uac_out_ms)  < LIVE_MS;
        bool mic_live     = (now - s_last_uac_in_ms)   < LIVE_MS;
        bool fail_live    = (now - s_last_now_fail_ms) < LIVE_MS;

        if (fail_live) {
            pulse_on = !pulse_on;
            rgb_set(pulse_on ? 255 : 0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else if (mic_live && spk_live) {
            rgb_set(255, 255, 255);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else if (spk_live) {
            rgb_set(255, 0, 255);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else if (mic_live) {
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
                 "uac_out=%" PRIu32 "B uac_in=%" PRIu32 "B "
                 "now_ok=%" PRIu32 " now_fail=%" PRIu32,
                 s_uac_out_bytes, s_uac_in_bytes, s_now_ok, s_now_fail);
        s_uac_out_bytes = 0;
        s_uac_in_bytes  = 0;
        s_now_ok        = 0;
        s_now_fail      = 0;
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
    ESP_LOGI(TAG,
             "Node_A_Spk_Mic_Test_idf "
             "(44.1 kHz stereo spk + 44.1 kHz mono silent mic, ESP-NOW ch %d)",
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
        .input_cb      = uac_input_cb,
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

    ESP_LOGI(TAG, "UAC ready (spk + silent mic, both @ 44.1 kHz)");

    xTaskCreatePinnedToCore(led_task,   "led",   4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(stats_task, "stats", 4096, NULL, 3, NULL, 1);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
