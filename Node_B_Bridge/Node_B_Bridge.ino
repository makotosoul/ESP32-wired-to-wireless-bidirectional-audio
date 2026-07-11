#define AUDIOKIT_BOARD 5

#include "AudioBoard.h"
#include "ESP_I2S.h"
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "../peers.h"

using namespace audio_driver;

/**
 * NODE B: Bidirectional bridge
 * - Mic: 48 kHz stereo I2S capture -> downsample 3x (L) -> 16 kHz mono ADPCM -> ESP-NOW (200 B / 25 ms)
 * - Speaker: ESP-NOW (240 B / 10 ms) -> 48 kHz mono ADPCM decode -> I2S stereo DAC
 *
 * Playback uses I2SClass + ES8388 DAC (same as Node_B_Audio). AudioBoardStream RX_MODE
 * does not drive kit.write(); using it for speaker was the reason host audio was silent.
 */

#define CAPTURE_RATE        48000
#define MIC_PKT_BYTES       200
#define SPK_PKT_BYTES       240
#define SPK_MONO_SAMPLES    480
#define CAPTURE_STEREO_PAIRS 1200
/* Split the 25 ms mic read into 5 ms chunks so speakerTask can grab the
 * shared i2sMutex between chunks. Single 25 ms readBytes() was holding the
 * mutex long enough to cap speaker playback at ~40 frames/s (vs ~100 needed). */
#define CAPTURE_CHUNKS         5
#define CAPTURE_CHUNK_PAIRS    (CAPTURE_STEREO_PAIRS / CAPTURE_CHUNKS)
#define CAPTURE_CHUNK_BYTES    (CAPTURE_CHUNK_PAIRS * 2 * (int)sizeof(int16_t))

/* Deep ISR queue so onDataRecv's xQueueSendFromISR essentially never drops.
 * An ISR-side drop = packet lost between Node A's encoder and Node B's decoder
 * = permanent IMA ADPCM state desync = static. 64 packets ~= 640 ms backlog. */
#define SPEAKER_QUEUE_DEPTH   64
#define SPK_PLAY_QUEUE_MAX    8
/* No vTaskDelayUntil pacing: i2s.write() is blocking and already paced by the
 * 48 kHz DAC clock. Calling vTaskDelayUntil with a zero increment asserts. */

I2SClass i2s;
QueueHandle_t speakerQueue;
SemaphoreHandle_t i2sMutex;

static const int indexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};
static const int stepsizeTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

struct ADPCMState { int32_t predictor; int8_t index; };
ADPCMState enc_mic = {0, 0};
ADPCMState dec_spk = {0, 0};

uint8_t encodeADPCM(int16_t sample, ADPCMState &state) {
    int32_t diff = sample - state.predictor;
    uint8_t code = 0;
    int32_t step = stepsizeTable[state.index];
    int32_t diffq = step >> 3;
    if (diff < 0) { code = 8; diff = -diff; }
    if (diff >= step) { code |= 4; diff -= step; diffq += step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; diffq += step; }
    step >>= 1;
    if (diff >= step) { code |= 1; diffq += step; }
    if (code & 8) state.predictor -= diffq; else state.predictor += diffq;
    if (state.predictor > 32767) state.predictor = 32767;
    else if (state.predictor < -32768) state.predictor = -32768;
    state.index += indexTable[code & 0x07];
    if (state.index < 0) state.index = 0;
    else if (state.index > 88) state.index = 88;
    return code;
}

int16_t decodeADPCM(uint8_t code, ADPCMState &state) {
    int32_t step = stepsizeTable[state.index];
    int32_t diffq = step >> 3;
    if (code & 4) diffq += step;
    if (code & 2) diffq += (step >> 1);
    if (code & 1) diffq += (step >> 2);
    if (code & 8) state.predictor -= diffq; else state.predictor += diffq;
    if (state.predictor > 32767) state.predictor = 32767;
    else if (state.predictor < -32768) state.predictor = -32768;
    state.index += indexTable[code & 0x07];
    if (state.index < 0) state.index = 0;
    else if (state.index > 88) state.index = 88;
    return (int16_t)state.predictor;
}

uint32_t mic_sent = 0;
uint32_t mic_fail = 0;
volatile uint32_t spk_frames = 0;
/* Diagnostics: see where the 60 missing speaker packets/sec are going. */
volatile uint32_t spk_recv = 0;          /* ESP-NOW packets received in onDataRecv */
volatile uint32_t spk_isr_drop = 0;      /* ISR-side queue full (silent drop, desyncs ADPCM) */
volatile uint32_t spk_dropthrough = 0;   /* Queue-trim drops handled by decode-through */

void onDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
    (void)tx_info;
    if (status == ESP_NOW_SEND_SUCCESS) mic_sent++;
    else mic_fail++;
}

void onDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
    (void)recv_info;
    if (len == SPK_PKT_BYTES) {
        spk_recv++;
        BaseType_t wake = pdFALSE;
        if (xQueueSendFromISR(speakerQueue, incomingData, &wake) != pdTRUE) {
            /* queue full — speakerTask starved. ADPCM desyncs after this. */
            spk_isr_drop++;
        } else if (wake == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

void speakerTask(void *pvParameters) {
    uint8_t compressed[SPK_PKT_BYTES];
    uint8_t drop_pkt[SPK_PKT_BYTES];
    int16_t mono[SPK_MONO_SAMPLES];
    int16_t stereo[SPK_MONO_SAMPLES * 2];

    while (1) {
        if (xQueueReceive(speakerQueue, compressed, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Trim queue to bound latency. CRITICAL: we must RUN THE DECODER
         * through every dropped packet, even though we discard the audio.
         * Node A's encoder already advanced its predictor through these
         * samples; if dec_spk doesn't advance in step, IMA ADPCM desyncs
         * permanently and the remaining stream becomes pure static. */
        while (uxQueueMessagesWaiting(speakerQueue) > SPK_PLAY_QUEUE_MAX) {
            if (xQueueReceive(speakerQueue, drop_pkt, 0) != pdTRUE) {
                break;
            }
            spk_dropthrough++;
            for (int i = 0; i < SPK_PKT_BYTES; i++) {
                (void)decodeADPCM(drop_pkt[i] & 0x0F,        dec_spk);
                (void)decodeADPCM((drop_pkt[i] >> 4) & 0x0F, dec_spk);
            }
        }

        for (int i = 0; i < SPK_PKT_BYTES; i++) {
            uint8_t low  = compressed[i] & 0x0F;
            uint8_t high = (compressed[i] >> 4) & 0x0F;
            mono[i * 2]     = decodeADPCM(low,  dec_spk);
            mono[i * 2 + 1] = decodeADPCM(high, dec_spk);
        }
        for (int i = 0; i < SPK_MONO_SAMPLES; i++) {
            stereo[i * 2]     = mono[i];
            stereo[i * 2 + 1] = mono[i];
        }

        /* i2s.write() is blocking and paced by the 48 kHz DAC clock — no
         * manual vTaskDelayUntil needed (and 0-increment vTaskDelayUntil
         * asserts on ESP-IDF 5.x). */
        if (xSemaphoreTake(i2sMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            i2s.write((uint8_t *)stereo, sizeof(stereo));
            xSemaphoreGive(i2sMutex);
            spk_frames++;
        }
    }
}

void micTask(void *pvParameters) {
    int16_t capture_buffer[CAPTURE_STEREO_PAIRS * 2];
    uint8_t compressed[MIC_PKT_BYTES];
    uint32_t last_print = 0;
    int32_t peak = 0;

    while (1) {
        /* Read the 25 ms packet as 5 x 5 ms chunks, releasing i2sMutex
         * between each chunk so speakerTask can play frames in the gaps. */
        bool all_chunks_read = true;
        for (int c = 0; c < CAPTURE_CHUNKS; c++) {
            char *chunk_ptr = (char *)(capture_buffer + c * CAPTURE_CHUNK_PAIRS * 2);
            size_t br = 0;
            if (xSemaphoreTake(i2sMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                br = i2s.readBytes(chunk_ptr, CAPTURE_CHUNK_BYTES);
                xSemaphoreGive(i2sMutex);
            }
            if ((int)br != CAPTURE_CHUNK_BYTES) {
                all_chunks_read = false;
                break;
            }
        }

        if (!all_chunks_read) {
            vTaskDelay(1);
            continue;
        }

        for (int i = 0; i < MIC_PKT_BYTES; i++) {
            int16_t sample1 = capture_buffer[i * 12];
            int16_t sample2 = capture_buffer[i * 12 + 6];

            if (abs(sample1) > peak) peak = abs(sample1);
            if (abs(sample2) > peak) peak = abs(sample2);

            uint8_t code1 = encodeADPCM(sample1, enc_mic);
            uint8_t code2 = encodeADPCM(sample2, enc_mic);
            compressed[i] = (code1 & 0x0F) | ((code2 & 0x0F) << 4);
        }

        if (millis() - last_print > 1000) {
            Serial.printf(
                "[Mic] Peak:%d Sent:%u Fails:%u | "
                "[Spk] recv:%u played:%u dropthru:%u isr_drop:%u qdepth:%u\n",
                peak, mic_sent, mic_fail,
                spk_recv, spk_frames, spk_dropthrough, spk_isr_drop,
                (unsigned)uxQueueMessagesWaiting(speakerQueue));
            peak = 0;
            mic_sent = 0;
            mic_fail = 0;
            spk_recv = 0;
            spk_frames = 0;
            spk_dropthrough = 0;
            spk_isr_drop = 0;
            last_print = millis();
        }

        esp_now_send(nodeAAddress, compressed, MIC_PKT_BYTES);
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);

    speakerQueue = xQueueCreate(SPEAKER_QUEUE_DEPTH, SPK_PKT_BYTES);
    i2sMutex = xSemaphoreCreateMutex();

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);

    CodecConfig cfg;
    cfg.input_device = ADC_INPUT_LINE2;
    cfg.output_device = DAC_OUTPUT_ALL;
    cfg.i2s.bits = BIT_LENGTH_16BITS;
    cfg.i2s.rate = RATE_48K;
    cfg.i2s.fmt = I2S_NORMAL;

    if (AudioKitEs8388V1.begin(cfg)) {
        Serial.println("Codec: 48 kHz ADC+DAC (LINE2 in, DAC out)");
    } else {
        Serial.println("Codec init FAILED");
    }
    AudioKitEs8388V1.setVolume(55);
    ::es8388_set_mic_gain(MIC_GAIN_0DB);

    // A1S Audio Kit V2.2 ES8388 pins:
    //   BCLK=27, LRCK=25, DSDIN (DAC in / ESP TX)=26, ASDOUT (ADC out / ESP RX)=35, MCLK=0
    // Earlier 'din=-1' broke mic capture (readBytes returned 0).
    i2s.setPins(27, 25, 26, 35, 0);
    i2s.begin(I2S_MODE_STD, CAPTURE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
              I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);

    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed");
        return;
    }
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataRecv);

    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, nodeAAddress, 6);
    peerInfo.channel = 1;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);

    xTaskCreatePinnedToCore(speakerTask, "SpeakerTask", 8192, NULL, 12, NULL, 0);
    xTaskCreatePinnedToCore(micTask, "MicTask", 8192, NULL, 8, NULL, 1);

    Serial.println("\n--- Node B Bridge: I2S duplex @ 48 kHz (16k mic / 48k spk ESP-NOW) ---");
}

void loop() {
    delay(1000);
}
