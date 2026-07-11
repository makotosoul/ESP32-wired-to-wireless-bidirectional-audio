#define AUDIOKIT_BOARD 5 

#include "AudioTools.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include "../peers.h"
#include "AudioBoard.h"
AudioBoardStream kit(AudioKitEs8388V1);
using namespace audio_tools;

/**
 * NODE B: TRANSMITTER
 * Captures at 48kHz Stereo (Hardware Stability)
 * Transmits at 16kHz Mono (Wireless Efficiency)
 */

#define CAPTURE_RATE    48000
#define PACKET_SIZE     200    
#define RAW_SAMPLES     400    

// --- ADPCM Encoder Logic ---
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
ADPCMState enc_state = {0, 0};

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
    if (state.predictor > 32767) state.predictor = 32767; else if (state.predictor < -32768) state.predictor = -32768;
    state.index += indexTable[code & 0x07];
    if (state.index < 0) state.index = 0; else if (state.index > 88) state.index = 88;
    return code;
}

uint32_t sent_count = 0;
uint32_t fail_count = 0;

void onDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) sent_count++;
    else fail_count++;
}

void micTask(void *pvParameters) {
    int16_t capture_buffer[1200 * 2]; // 25ms @ 48kHz Stereo
    uint8_t compressed[PACKET_SIZE];
    uint32_t last_print = 0;
    int32_t peak = 0;
    
    while (1) {
        size_t bytes_read = kit.readBytes((uint8_t*)capture_buffer, sizeof(capture_buffer));
        
        if (bytes_read == sizeof(capture_buffer)) {
            for (int i = 0; i < PACKET_SIZE; i++) {
                // Downsample 3x: 48kHz -> 16kHz
                int16_t sample1 = capture_buffer[i * 12];
                int16_t sample2 = capture_buffer[i * 12 + 6];
                
                if (abs(sample1) > peak) peak = abs(sample1);
                if (abs(sample2) > peak) peak = abs(sample2);
                
                uint8_t code1 = encodeADPCM(sample1, enc_state);
                uint8_t code2 = encodeADPCM(sample2, enc_state);
                
                compressed[i] = (code1 & 0x0F) | ((code2 & 0x0F) << 4);
            }
            
            if (millis() - last_print > 1000) {
                Serial.printf("[TX] Peak: %d | Sent: %d | Fails: %d\n", peak, sent_count, fail_count);
                peak = 0; sent_count = 0; fail_count = 0;
                last_print = millis();
            }
            
            esp_now_send(nodeAAddress, compressed, PACKET_SIZE);
        } else {
            vTaskDelay(1); 
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);

    AudioLogger::instance().begin(Serial, AudioLogger::Warning);
    
    auto cfg = kit.defaultConfig(RX_MODE);
    cfg.sample_rate = CAPTURE_RATE;
    cfg.channels = 2; 
    cfg.input_device = audio_driver::ADC_INPUT_LINE2; 
    kit.begin(cfg);
    
    ::es8388_set_mic_gain(audio_driver::MIC_GAIN_0DB);

    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("Error initializing ESP-NOW");
        return;
    }
    esp_now_register_send_cb(onDataSent);
    
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, nodeAAddress, 6);
    peerInfo.channel = 1;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);

    xTaskCreatePinnedToCore(micTask, "MicTask", 8192, NULL, 10, NULL, 1);
}

void loop() { delay(1000); }