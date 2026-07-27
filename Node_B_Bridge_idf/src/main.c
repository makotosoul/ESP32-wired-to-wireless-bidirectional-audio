/*
 * Node_B_Bridge_idf
 *
 * A1S Audio Kit (ESP32, ES8388) end of the bidirectional bridge.
 *
 * Migrated from Arduino sketch (Node_B_Bridge/) because the Arduino I2SClass
 * serialised TX and RX internally, capping speaker playback at ~40 frames/sec
 * even with chunked mic reads. The ESP-IDF i2s_std driver supports true
 * full-duplex with separate TX and RX channels (independent DMA), so the
 * speaker can hit the full 100 frames/sec (one 10 ms frame per 10 ms) regardless
 * of mic activity. The i2sMutex from the Arduino version is gone.
 *
 * Audio paths:
 *   Spk: ESP-NOW <- Node A: Opus v2 (or legacy 240 B ADPCM) / 10 ms
 *        -> 480 mono samples -> duplicate L=R -> i2s @ 48 kHz stereo
 *   Mic: i2s_channel_read @ 48 kHz stereo -> downsample 3x (every 6th L)
 *        -> 16 kHz mono -> 200 B ADPCM / 25 ms -> ESP-NOW -> Node A
 *
 * ES8388 codec: configured via espressif/esp_codec_dev managed component.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "nvs_flash.h"

#include "driver/i2s_std.h"
#include "driver/i2c_master.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "opus.h"
#include "bridge_spk.h"
#include "peers.h"

static const char *TAG = "bridge_b";

/* Node A MAC — from peers.h (update via auto_mac_setup.sh if boards change). */
static const uint8_t NODE_A_ADDR[6] = {PEER_MAC_A};

#define ESP_NOW_CHANNEL         1

/* Audio rates / packet sizes — MUST match Node_A_Bridge_idf. */
#define SAMPLE_RATE             48000
#define SPK_MONO_SAMPLES        BRIDGE_SPK_FRAME_SAMPLES
#define MIC_PKT_BYTES           200
#define CAPTURE_STEREO_PAIRS    1200    /* 25 ms @ 48 kHz stereo (1200 L+R pairs) */

/* Spk queue: deep so on_now_recv -> xQueueSend essentially never overflows
 * (an ISR-side drop would permanently desync IMA ADPCM = static). */
#define SPK_QUEUE_DEPTH         64
#define SPK_PLAY_QUEUE_MAX      8       /* trim to bound latency at ~80 ms */

/* I2S pins (A1S Audio Kit V2.2 ES8388). */
#define I2S_BCLK_GPIO           GPIO_NUM_27
#define I2S_LRCK_GPIO           GPIO_NUM_25
#define I2S_DOUT_GPIO           GPIO_NUM_26     /* MCU -> ES8388 DAC IN  (DSDIN) */
#define I2S_DIN_GPIO            GPIO_NUM_35     /* MCU <- ES8388 ADC OUT (ASDOUT) */
#define I2S_MCLK_GPIO           GPIO_NUM_0      /* ES8388 master clock */

/* DMA: 8 descriptors x 240 frames = 1920 frames per direction = 40 ms buffering.
 * Plenty of headroom so i2s_channel_write rarely blocks long under load. */
#define I2S_DMA_DESC_NUM        8
#define I2S_DMA_FRAME_NUM       240

/* I2C (ES8388 control bus). */
#define I2C_PORT                I2C_NUM_0
#define I2C_SDA_GPIO            GPIO_NUM_33
#define I2C_SCL_GPIO            GPIO_NUM_32
#define ES8388_I2C_ADDR_7BIT    0x10

/* ES8388 register addresses we touch directly (post-codec_open) to match the
 * A1S board wiring. esp_codec_dev's default ADC input is LINPUT1/RINPUT1,
 * but the A1S V2.2 routes the mic header to LINPUT2/RINPUT2. */
#define ES8388_REG_ADCCONTROL2  0x0a
#define ES8388_VAL_INPUT_LINE2  0x50    /* LINSEL=01 (LIN2), RINSEL=01 (RIN2) */

/* A1S V2.2 PA enable pin (drives the on-board speaker amp). */
#define PA_ENABLE_GPIO          GPIO_NUM_21

/* esp_codec_dev output level 0..100 (default curve: 0 -> -50 dB, 100 -> 0 dB). */
#define DAC_OUT_VOLUME          100

/* ES8388 LOUT/ROUT mixer gains (DACCONTROL24/25): 0x1E = 0 dB, 0x21 = +3 dB (driver max). */
#define ES8388_REG_DACCONTROL24 0x2e
#define ES8388_REG_DACCONTROL25 0x2f
#define ES8388_LOUT_GAIN        0x21

/* Post-decode PCM boost before I2S (2^SHIFT). ADPCM + quiet host levels often need this;
 * reduce to 1 if you hear clipping/distortion. */
#define SPK_PCM_GAIN_SHIFT      2

/* Mic PGA via esp_codec_dev (dB). Arduino bridge used 0 dB; IDF default was 24 dB.
 * −6 dB halves linear level; tune 0–24 here. */
#define MIC_IN_GAIN_DB          0.0f

/* ====================== Globals ====================== */
static i2c_master_bus_handle_t  s_i2c_bus;
static const audio_codec_ctrl_if_t *s_codec_ctrl;
static i2s_chan_handle_t        s_tx_chan;
static i2s_chan_handle_t        s_rx_chan;
static esp_codec_dev_handle_t   s_codec_dev;
typedef struct {
    uint8_t len;
    uint8_t data[BRIDGE_SPK_MAX_PKT_BYTES];
} spk_queue_item_t;

static QueueHandle_t            s_spk_queue;
static OpusDecoder             *s_opus_dec;

typedef struct {
    int32_t predictor;
    int8_t  index;
} adpcm_state_t;

static adpcm_state_t            s_enc_mic = {0, 0};
static adpcm_state_t            s_dec_spk = {0, 0};

/* Stats printed once / sec by mic task. */
static volatile uint32_t        s_mic_sent;
static volatile uint32_t        s_mic_fail;
static volatile uint32_t        s_spk_recv;
static volatile uint32_t        s_spk_played;
static volatile uint32_t        s_spk_dropthrough;
static volatile uint32_t        s_spk_isr_drop;
static volatile uint32_t        s_opus_ok;
static volatile uint32_t        s_opus_fail;
static volatile int32_t         s_spk_peak;

/* ====================== IMA ADPCM ====================== */
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
    else st->predictor += diffq;
    if (st->predictor >  32767) st->predictor =  32767;
    if (st->predictor < -32768) st->predictor = -32768;
    st->index += s_indexTable[code & 0x07];
    if (st->index <  0) st->index = 0;
    if (st->index > 88) st->index = 88;
    return code;
}

/* ====================== ESP-NOW ====================== */
static void on_now_send(const uint8_t *mac, esp_now_send_status_t status)
{
    (void)mac;
    if (status == ESP_NOW_SEND_SUCCESS) s_mic_sent++;
    else                                s_mic_fail++;
}

static void on_now_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    (void)info;
    if (!bridge_spk_is_adpcm_pkt(len) && !bridge_spk_is_opus_pkt(data, len)) {
        return;
    }
    s_spk_recv++;
    spk_queue_item_t item = {0};
    item.len = (uint8_t)len;
    memcpy(item.data, data, (size_t)len);
    /* recv callback runs in WiFi task context (not ISR) on ESP-IDF 5.x. */
    if (xQueueSend(s_spk_queue, &item, 0) != pdTRUE) {
        s_spk_isr_drop++;
    }
}

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
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_now_recv));

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, NODE_A_ADDR, 6);
    peer.channel = ESP_NOW_CHANNEL;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

/* ====================== I2C / I2S / ES8388 ====================== */
static esp_err_t i2c_bus_init(void)
{
    /* esp_codec_dev on IDF 5.3+ uses the new I2C master driver and requires
     * bus_handle (legacy i2c_driver_install alone makes audio_codec_new_i2c_ctrl
     * return NULL and ESP_ERROR_CHECK in app_main reboots the chip). */
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port   = I2C_PORT,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* ES8388 has no codec_if->set_reg; esp_codec_dev_write_reg() is a no-op for this chip.
 * Use the I2C ctrl interface created for esp_codec_dev instead. */
static esp_err_t es8388_reg_write(uint8_t reg, uint8_t val)
{
    if (s_codec_ctrl == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int rc = s_codec_ctrl->write_reg(s_codec_ctrl, reg, 1, &val, 1);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t i2s_init(void)
{
    /* Full-duplex: a single call to i2s_new_channel allocates BOTH TX and RX
     * channel handles, sharing the same I2S0 BCLK/LRCK but with independent
     * DMA queues. This is the key win over the Arduino I2SClass. */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;
    chan_cfg.auto_clear    = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel (full-duplex) failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_GPIO,
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_LRCK_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_DIN_GPIO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode(tx) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode(rx) failed: %s", esp_err_to_name(err));
        return err;
    }
    /* Channels enabled by esp_codec_dev_open via the i2s_data interface. */
    return ESP_OK;
}

static esp_err_t codec_init(void)
{
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = I2C_PORT,
        .addr       = ES8388_I2C_ADDR_7BIT << 1,   /* 8-bit write addr */
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed");
        return ESP_FAIL;
    }
    s_codec_ctrl = ctrl_if;

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_gpio failed");
        return ESP_FAIL;
    }

    /* es8388_codec_cfg_t in esp_codec_dev v1.3.x has no input-selection field.
     * We pick the default (LINE1) here, then override register 0x0a below to
     * switch to LINE2 (the mic header on the A1S Audio Kit V2.2). */
    es8388_codec_cfg_t es_cfg = {
        .ctrl_if            = ctrl_if,
        .gpio_if            = gpio_if,
        .codec_mode         = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin             = PA_ENABLE_GPIO,
        .pa_reverted        = false,
        .master_mode        = false,
        .hw_gain.pa_voltage        = 5.0f,
        .hw_gain.codec_dac_voltage = 3.3f,
    };
    const audio_codec_if_t *codec_if = es8388_codec_new(&es_cfg);
    if (codec_if == NULL) {
        ESP_LOGE(TAG, "es8388_codec_new failed");
        return ESP_FAIL;
    }

    audio_codec_i2s_cfg_t data_cfg = {
        .port       = I2S_NUM_0,
        .rx_handle  = s_rx_chan,
        .tx_handle  = s_tx_chan,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_cfg);
    if (data_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec_if,
        .data_if  = data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    if (s_codec_dev == NULL) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = SAMPLE_RATE,
        .channel         = 2,
        .bits_per_sample = 16,
    };
    int rc = esp_codec_dev_open(s_codec_dev, &fs);
    if (rc != 0) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed (%d)", rc);
        return ESP_FAIL;
    }

    rc = esp_codec_dev_set_out_vol(s_codec_dev, DAC_OUT_VOLUME);
    if (rc != 0) {
        ESP_LOGW(TAG, "set_out_vol(%d) returned %d (continuing)", DAC_OUT_VOLUME, rc);
    }
    rc = esp_codec_dev_set_in_gain(s_codec_dev, MIC_IN_GAIN_DB);
    if (rc != 0) {
        ESP_LOGW(TAG, "set_in_gain(%.0f dB) returned %d (continuing)", MIC_IN_GAIN_DB, rc);
    }

    /* esp_codec_dev_open leaves ADCCONTROL2 on LINE1; A1S mic jack is LINE2. */
    if (es8388_reg_write(ES8388_REG_ADCCONTROL2, ES8388_VAL_INPUT_LINE2) != ESP_OK) {
        ESP_LOGE(TAG, "ADCCONTROL2 LINE2 select failed — mic will be silent on this board");
    }

    /* Output-stage gain (separate from DACCONTROL4/5 digital volume). */
    if (es8388_reg_write(ES8388_REG_DACCONTROL24, ES8388_LOUT_GAIN) != ESP_OK) {
        ESP_LOGW(TAG, "DACCONTROL24 LOUT gain write failed");
    }
    if (es8388_reg_write(ES8388_REG_DACCONTROL25, ES8388_LOUT_GAIN) != ESP_OK) {
        ESP_LOGW(TAG, "DACCONTROL25 ROUT gain write failed");
    }

    ESP_LOGI(TAG,
             "ES8388 ready: 48 kHz, LINE2 in, vol=%d, LOUT=+3dB, PCM shift=%d, mic_gain=%.0fdB",
             DAC_OUT_VOLUME, SPK_PCM_GAIN_SHIFT, MIC_IN_GAIN_DB);
    return ESP_OK;
}

static esp_err_t spk_opus_decoder_init(void)
{
    int err = OPUS_OK;
    s_opus_dec = opus_decoder_create(BRIDGE_SPK_SAMPLE_RATE_HZ, 1, &err);
    if (err != OPUS_OK || s_opus_dec == NULL) {
        ESP_LOGE(TAG, "opus_decoder_create failed: %s", opus_strerror(err));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Opus speaker decoder: %d Hz mono", BRIDGE_SPK_SAMPLE_RATE_HZ);
    return ESP_OK;
}

/* One-shot encoder to build a valid 10 ms frame; encoder freed before return
 * (NONTHREADSAFE pseudostack is shared with the decoder). */
static esp_err_t spk_opus_warmup_decode(void)
{
    static int16_t pcm[SPK_MONO_SAMPLES];
    static uint8_t pkt[BRIDGE_SPK_MAX_PKT_BYTES];
    int enc_err = OPUS_OK;
    OpusEncoder *enc = opus_encoder_create(BRIDGE_SPK_SAMPLE_RATE_HZ, 1,
                                           OPUS_APPLICATION_AUDIO, &enc_err);
    if (enc_err != OPUS_OK || enc == NULL) {
        ESP_LOGE(TAG, "warmup opus_encoder_create: %s", opus_strerror(enc_err));
        return ESP_FAIL;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(BRIDGE_SPK_OPUS_BITRATE));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(BRIDGE_SPK_OPUS_COMPLEXITY_ENC));
    opus_encoder_ctl(enc, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
    opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));

    memset(pcm, 0, sizeof(pcm));
    int opus_bytes = opus_encode(enc, pcm, SPK_MONO_SAMPLES,
                                 &pkt[BRIDGE_SPK_OPUS_HDR_BYTES],
                                 BRIDGE_SPK_MAX_PKT_BYTES - BRIDGE_SPK_OPUS_HDR_BYTES);
    opus_encoder_destroy(enc);
    if (opus_bytes < 0) {
        ESP_LOGE(TAG, "warmup opus_encode: %s", opus_strerror(opus_bytes));
        return ESP_FAIL;
    }

    int samples = opus_decode(s_opus_dec, &pkt[BRIDGE_SPK_OPUS_HDR_BYTES], opus_bytes,
                              pcm, SPK_MONO_SAMPLES, 0);
    if (samples != SPK_MONO_SAMPLES) {
        ESP_LOGE(TAG, "warmup opus_decode: %s",
                 samples < 0 ? opus_strerror(samples) : "bad sample count");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Opus warmup OK (%d B frame)", opus_bytes);
    return ESP_OK;
}

static int spk_decode_packet(const spk_queue_item_t *item, int16_t *mono)
{
    if (bridge_spk_is_opus_pkt(item->data, item->len)) {
        const uint8_t *opus = &item->data[BRIDGE_SPK_OPUS_HDR_BYTES];
        int opus_len = (int)item->data[1];
        int samples = opus_decode(s_opus_dec, opus, opus_len, mono, SPK_MONO_SAMPLES, 0);
        if (samples != SPK_MONO_SAMPLES) {
            if (samples < 0) {
                ESP_LOGW(TAG, "opus_decode: %s", opus_strerror(samples));
            }
            s_opus_fail++;
            return -1;
        }
        s_opus_ok++;
        return 0;
    }

    if (!bridge_spk_is_adpcm_pkt(item->len)) {
        return -1;
    }

    for (int i = 0; i < BRIDGE_SPK_ADPCM_PKT_BYTES; i++) {
        mono[i * 2]     = adpcm_decode(item->data[i] & 0x0F,        &s_dec_spk);
        mono[i * 2 + 1] = adpcm_decode((item->data[i] >> 4) & 0x0F, &s_dec_spk);
    }
    return 0;
}

static void spk_dropthrough_adpcm(const spk_queue_item_t *item)
{
    if (!bridge_spk_is_adpcm_pkt(item->len)) {
        return;
    }
    for (int i = 0; i < BRIDGE_SPK_ADPCM_PKT_BYTES; i++) {
        (void)adpcm_decode(item->data[i] & 0x0F,        &s_dec_spk);
        (void)adpcm_decode((item->data[i] >> 4) & 0x0F, &s_dec_spk);
    }
}

static void spk_pcm_to_stereo_and_play(const int16_t *mono)
{
    static int16_t stereo[SPK_MONO_SAMPLES * 2];

    for (int i = 0; i < SPK_MONO_SAMPLES; i++) {
        int32_t s = ((int32_t)mono[i]) << SPK_PCM_GAIN_SHIFT;
        if (s > 32767) {
            s = 32767;
        } else if (s < -32768) {
            s = -32768;
        }
        if (abs((int)s) > s_spk_peak) {
            s_spk_peak = abs((int)s);
        }
        stereo[i * 2]     = (int16_t)s;
        stereo[i * 2 + 1] = (int16_t)s;
    }

    size_t written = 0;
    esp_err_t err = i2s_channel_write(s_tx_chan, stereo, sizeof(stereo),
                                      &written, pdMS_TO_TICKS(100));
    if (err == ESP_OK && written == sizeof(stereo)) {
        s_spk_played++;
    }
}

/* ====================== Speaker task ====================== */
static void speaker_task(void *arg)
{
    (void)arg;
    static spk_queue_item_t item;
    static spk_queue_item_t drop_item;
    static int16_t          mono[SPK_MONO_SAMPLES];

    for (;;) {
        if (xQueueReceive(s_spk_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Trim queue to bound latency. ADPCM drops must decode-through; Opus
         * frames are independent and can be discarded without state sync. */
        while (uxQueueMessagesWaiting(s_spk_queue) > SPK_PLAY_QUEUE_MAX) {
            if (xQueueReceive(s_spk_queue, &drop_item, 0) != pdTRUE) {
                break;
            }
            s_spk_dropthrough++;
            spk_dropthrough_adpcm(&drop_item);
        }

        if (spk_decode_packet(&item, mono) != 0) {
            memset(mono, 0, SPK_MONO_SAMPLES * sizeof(int16_t));
        }
        spk_pcm_to_stereo_and_play(mono);
        vTaskDelay(1);
    }
}

/* ====================== Mic task ====================== */
static void mic_task(void *arg)
{
    (void)arg;
    static int16_t  capture_buffer[CAPTURE_STEREO_PAIRS * 2];
    static uint8_t  compressed[MIC_PKT_BYTES];
    int32_t  peak = 0;
    int32_t  spk_peak_snap = 0;
    uint32_t opus_ok_snap = 0;
    uint32_t opus_fail_snap = 0;
    uint32_t last_print_ms = 0;

    for (;;) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, capture_buffer,
                                         sizeof(capture_buffer), &bytes_read,
                                         portMAX_DELAY);
        if (err != ESP_OK || bytes_read != sizeof(capture_buffer)) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        /* Downsample 48 kHz stereo -> 16 kHz mono by picking L every 3 stereo
         * pairs (= every 6 int16 entries). 200 iterations * 2 samples = 400
         * mono samples = 200 ADPCM bytes (4 bits/sample). */
        for (int i = 0; i < MIC_PKT_BYTES; i++) {
            int16_t s1 = capture_buffer[i * 12];
            int16_t s2 = capture_buffer[i * 12 + 6];
            if (abs(s1) > peak) peak = abs(s1);
            if (abs(s2) > peak) peak = abs(s2);
            uint8_t c1 = adpcm_encode(s1, &s_enc_mic);
            uint8_t c2 = adpcm_encode(s2, &s_enc_mic);
            compressed[i] = (c1 & 0x0F) | ((c2 & 0x0F) << 4);
        }

        if (esp_now_send(NODE_A_ADDR, compressed, MIC_PKT_BYTES) != ESP_OK) {
            s_mic_fail++;
        }

        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now_ms - last_print_ms > 1000) {
            spk_peak_snap = s_spk_peak;
            s_spk_peak = 0;
            opus_ok_snap = s_opus_ok;
            s_opus_ok = 0;
            opus_fail_snap = s_opus_fail;
            s_opus_fail = 0;
            ESP_LOGI(TAG,
                "[Mic] Peak:%" PRId32 " Sent:%" PRIu32 " Fails:%" PRIu32
                " | [Spk] Peak:%" PRId32 " recv:%" PRIu32 " played:%" PRIu32
                " opus_ok:%" PRIu32 " opus_fail:%" PRIu32
                " dropthru:%" PRIu32 " isr_drop:%" PRIu32 " qdepth:%u",
                peak, s_mic_sent, s_mic_fail,
                spk_peak_snap, s_spk_recv, s_spk_played,
                opus_ok_snap, opus_fail_snap,
                s_spk_dropthrough, s_spk_isr_drop,
                (unsigned)uxQueueMessagesWaiting(s_spk_queue));
            peak = 0;
            s_mic_sent = 0;
            s_mic_fail = 0;
            s_spk_recv = 0;
            s_spk_played = 0;
            s_spk_dropthrough = 0;
            s_spk_isr_drop = 0;
            last_print_ms = now_ms;
        }
    }
}

/* ====================== app_main ====================== */
void app_main(void)
{
    ESP_LOGI(TAG, "Node_B_Bridge_idf (full-duplex I2S, ES8388 via esp_codec_dev)");

    wifi_espnow_init();

    s_spk_queue = xQueueCreate(SPK_QUEUE_DEPTH, sizeof(spk_queue_item_t));
    if (s_spk_queue == NULL) {
        ESP_LOGE(TAG, "spk_queue alloc failed");
        return;
    }

    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_ERROR_CHECK(i2s_init());
    ESP_ERROR_CHECK(codec_init());

    ESP_LOGI(TAG, "heap SPIRAM free=%u internal free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (spk_opus_decoder_init() != ESP_OK || spk_opus_warmup_decode() != ESP_OK) {
        ESP_LOGE(TAG, "Opus decoder init/warmup failed on WROOM — revert Node A to ADPCM");
        return;
    }

    /* Speaker on Core 1 (alone) — dedicates a core to the 100 Hz playback loop.
     * Mic on Core 0 with WiFi — mic packet rate is only 40 Hz so contention is
     * trivial; co-locating with WiFi keeps esp_now_send cheap. */
    xTaskCreatePinnedToCore(speaker_task, "spk", 16384, NULL, 12, NULL, 1);
    xTaskCreatePinnedToCore(mic_task,     "mic", 8192, NULL, 8,  NULL, 0);

    ESP_LOGI(TAG, "Tasks running. spk Core 1 (prio 12), mic Core 0 (prio 8).");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
