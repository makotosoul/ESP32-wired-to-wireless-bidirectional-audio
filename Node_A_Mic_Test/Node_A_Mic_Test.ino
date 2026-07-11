#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include "USB.h"
#include "USBAudioCard.h"
#include "AudioTools.h"
#include "../peers.h"

using namespace audio_tools;

/**
 * NODE A: DONGLE (RECEIVER)
 * Optimized for 16kHz Mono - The most stable mode for Linux/Audacity.
 */

// 16kHz Mono 16-bit
USBAudioCard AudioCard(16000, UAC_BPS_16, UAC_SPK_NONE, UAC_MIC_MONO);

#define PACKET_SIZE 200

// --- IMA ADPCM Decoder Logic ---
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
ADPCMState dec_state = {0, 0};

int16_t decodeADPCM(uint8_t code, ADPCMState &state) {
    int32_t step = stepsizeTable[state.index];
    int32_t diffq = step >> 3;
    if (code & 4) diffq += step;
    if (code & 2) diffq += (step >> 1);
    if (code & 1) diffq += (step >> 2);
    if (code & 8) state.predictor -= diffq; else state.predictor += diffq;
    if (state.predictor > 32767) state.predictor = 32767; else if (state.predictor < -32768) state.predictor = -32768;
    state.index += indexTable[code & 0x07];
    if (state.index < 0) state.index = 0; else if (state.index > 88) state.index = 88;
    return (int16_t)state.predictor;
}

QueueHandle_t micQueue;

void onDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (len == PACKET_SIZE) {
        xQueueSendFromISR(micQueue, data, NULL);
    }
}

void micTask(void *pvParameters) {
    uint8_t compressed[PACKET_SIZE];
    int16_t pcm[400]; // 25ms @ 16kHz Mono
    
    while (1) {
        // Try to get data from wireless queue
        if (xQueueReceive(micQueue, compressed, 0)) {
            for (int i = 0; i < PACKET_SIZE; i++) {
                uint8_t low = compressed[i] & 0x0F;
                uint8_t high = (compressed[i] >> 4) & 0x0F;
                pcm[i*2] = decodeADPCM(low, dec_state);
                pcm[i*2+1] = decodeADPCM(high, dec_state);
            }
            // Send audio data to PC
            AudioCard.write((uint8_t*)pcm, sizeof(pcm));
        } else {
            // HEARTBEAT: Send 1ms of silence to keep USB clock moving
            // This prevents Audacity from stalling when no signal is present.
            int16_t silence[16]; 
            memset(silence, 0, sizeof(silence));
            AudioCard.write((uint8_t*)silence, sizeof(silence));
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

void setup() {
    USB.begin();
    AudioCard.begin();
    
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    micQueue = xQueueCreate(20, PACKET_SIZE);

    if (esp_now_init() == ESP_OK) {
        esp_now_register_recv_cb(onDataRecv);
    }

    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, nodeBAddress, 6);
    peerInfo.channel = 1;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);

    // Pin to Core 1 to separate Audio/USB from WiFi (Core 0)
    xTaskCreatePinnedToCore(micTask, "MicTask", 8192, NULL, 15, NULL, 1);
}

void loop() { delay(1000); }