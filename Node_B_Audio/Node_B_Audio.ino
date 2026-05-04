#include "AudioBoard.h"
#include "ESP_I2S.h"
#include <esp_now.h>
#include <WiFi.h>
#include "../peers.h"

using namespace audio_driver;

I2SClass i2s;
QueueHandle_t audioQueue;
#define QUEUE_SIZE 40 

// --- IMA ADPCM Decoder State ---
static const int indexTable[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int stepsizeTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

struct ADPCMState { int32_t predictor; int8_t index; };
ADPCMState dec_state_L = {0, 0};
ADPCMState dec_state_R = {0, 0};

int16_t decodeADPCM(uint8_t code, ADPCMState &state) {
    int32_t step = stepsizeTable[state.index];
    int32_t diffq = step >> 3;
    if (code & 4) diffq += step;
    if (code & 2) diffq += (step >> 1);
    if (code & 1) diffq += (step >> 2);
    if (code & 8) state.predictor -= diffq; else state.predictor += diffq;
    if (state.predictor > 32767) state.predictor = 32767; else if (state.predictor < -32768) state.predictor = -32768;
    state.index += indexTable[code & 0x07]; // Use only the bottom 3 bits for index lookup
    if (state.index < 0) state.index = 0; else if (state.index > 88) state.index = 88;
    return (int16_t)state.predictor;
}

volatile uint32_t decoded_samples_count = 0;
uint32_t last_report_ms = 0;

void onDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
    xQueueSendFromISR(audioQueue, incomingData, NULL);
}

void i2sTask(void *pvParameters) {
    uint8_t compressed_packet[240];
    int16_t stereo_buffer[240 * 2]; // 240 pairs (L, R)

    while (1) {
        if (xQueueReceive(audioQueue, compressed_packet, portMAX_DELAY)) {
            for (int i = 0; i < 240; i++) {
                // Each byte contains codeL (low) and codeR (high)
                uint8_t codeL = compressed_packet[i] & 0x0F;
                uint8_t codeR = (compressed_packet[i] >> 4) & 0x0F;
                
                stereo_buffer[i*2]     = decodeADPCM(codeL, dec_state_L);
                stereo_buffer[i*2 + 1] = decodeADPCM(codeR, dec_state_R);
            }
            decoded_samples_count += 240; 
            i2s.write((uint8_t*)stereo_buffer, sizeof(stereo_buffer));
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    audioQueue = xQueueCreate(QUEUE_SIZE, 240);

    CodecConfig cfg;
    cfg.input_device = ADC_INPUT_NONE; 
    cfg.output_device = DAC_OUTPUT_ALL;
    cfg.i2s.bits = BIT_LENGTH_16BITS;
    cfg.i2s.rate = RATE_44K; 
    cfg.i2s.fmt = I2S_NORMAL; 
    
    if (AudioKitEs8388V1.begin(cfg)) {
        Serial.println("Codec: SUCCESS (44.1kHz STEREO ADPCM)");
    }
    // Lower volume to 45 for cleaner 'Headroom' (Prevents clipping/distortion)
    AudioKitEs8388V1.setVolume(45); 

    i2s.setPins(27, 25, 26, -1, 0); 
    i2s.begin(I2S_MODE_STD, 44100, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);

    xTaskCreatePinnedToCore(i2sTask, "I2STask", 8192, NULL, 10, NULL, 0);

    WiFi.mode(WIFI_STA);
    esp_now_init();
    esp_now_register_recv_cb(onDataRecv);

    Serial.println("\n--- TRUE STEREO ADPCM DECODER READY ---");
}

void loop() {
    if (millis() - last_report_ms > 1000) {
        uint32_t samples = decoded_samples_count;
        decoded_samples_count = 0;
        last_report_ms = millis();
        float quality_kbps = (samples * 2.0 * 16.0) / 1000.0; // Stereo = samples * 2
        Serial.printf("[Node B] Receiving: 44.1kHz STEREO | Raw Fidelity: %.1f kbps\n", quality_kbps);
    }
}
