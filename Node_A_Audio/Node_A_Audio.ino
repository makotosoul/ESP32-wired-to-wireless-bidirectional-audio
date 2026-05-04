#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "USB.h"
#include "USBAudioCard.h"
#include "../peers.h"

#define SAMPLE_RATE     44100  
#define PACKET_SIZE     240    // 240 bytes = 480 nibbles (240 L + 240 R)
#define RAW_SAMPLES     480    // 240 stereo pairs (L, R, L, R...)

uint8_t* remoteAddress = nodeBAddress;

// 1. Enable STEREO USB Speaker
USBAudioCard AudioCard(SAMPLE_RATE, UAC_BPS_16, UAC_SPK_STEREO, UAC_MIC_NONE);

// --- IMA ADPCM State & Tables ---
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
ADPCMState enc_state_L = {0, 0};
ADPCMState enc_state_R = {0, 0};

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
    state.index += indexTable[code];
    if (state.index < 0) state.index = 0; else if (state.index > 88) state.index = 88;
    return code;
}

int16_t raw_buffer[RAW_SAMPLES]; // 240 L/R pairs
size_t raw_index = 0;
uint8_t compressed_packet[PACKET_SIZE];

void onSpeakerData(void *data, uint16_t len) {
    int16_t* incoming = (int16_t*)data;
    size_t count = len / 2; // Each 16-bit word is 2 bytes

    for (size_t i = 0; i < count; i++) {
        raw_buffer[raw_index++] = incoming[i];
        
        if (raw_index >= RAW_SAMPLES) {
            // Encode interleaved buffer
            for (int j = 0; j < RAW_SAMPLES; j += 2) {
                // j = Left, j+1 = Right
                uint8_t codeL = encodeADPCM(raw_buffer[j], enc_state_L);
                uint8_t codeR = encodeADPCM(raw_buffer[j+1], enc_state_R);
                compressed_packet[j/2] = (codeR << 4) | (codeL & 0x0F);
            }
            esp_now_send(remoteAddress, compressed_packet, PACKET_SIZE);
            raw_index = 0;
        }
    }
}

void setup() {
    Serial.begin(115200);
    AudioCard.onData(onSpeakerData);
    AudioCard.begin();
    USB.begin();

    WiFi.mode(WIFI_STA);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    esp_now_init();

    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, remoteAddress, 6);
    peerInfo.channel = 1;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);

    Serial.println("[Node A] 44.1kHz STEREO ADPCM Source Ready.");
}

void loop() {
    delay(1000);
}
