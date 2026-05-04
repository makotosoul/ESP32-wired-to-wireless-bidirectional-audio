#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "USB.h"
#include "USBAudioCard.h"
#include "../peers.h"

// --- High Fidelity Upgrade ---
#define SAMPLE_RATE     44100  // YouTube/CD Quality
#define PACKET_SIZE     240    // Keep ESP-NOW happy
#define TONE_MODE_PIN   0      

uint8_t* remoteAddress = nodeBAddress;

// 1. Audio Out Only (Speaker Yes, Mic No)
USBAudioCard AudioCard(SAMPLE_RATE, UAC_BPS_16, UAC_SPK_MONO, UAC_MIC_NONE);

int16_t out_acc_buffer[PACKET_SIZE / 2];
size_t out_acc_index = 0;

void onSpeakerData(void *data, uint16_t len) {
  if (digitalRead(TONE_MODE_PIN) == LOW) return;

  int16_t* incoming_samples = (int16_t*)data;
  size_t num_samples = len / 2;

  for (size_t i = 0; i < num_samples; i++) {
    out_acc_buffer[out_acc_index++] = incoming_samples[i];
    
    if (out_acc_index >= (PACKET_SIZE / 2)) {
      esp_now_send(remoteAddress, (uint8_t*)out_acc_buffer, PACKET_SIZE);
      out_acc_index = 0;
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(TONE_MODE_PIN, INPUT_PULLUP);
  
  AudioCard.onData(onSpeakerData);
  AudioCard.begin();
  USB.begin();

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return;

  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, remoteAddress, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  Serial.println("[Node A] 44.1kHz High-Fidelity Source Ready.");
}

float phase = 0;
float phase_inc = 2.0 * PI * 440.0 / SAMPLE_RATE;

void loop() {
  if (digitalRead(TONE_MODE_PIN) == LOW) {
    for (int i = 0; i < (PACKET_SIZE / 2); i++) {
      out_acc_buffer[i] = (int16_t)(10000.0 * sin(phase));
      phase += phase_inc;
      if (phase > 2.0 * PI) phase -= 2.0 * PI;
    }
    esp_now_send(remoteAddress, (uint8_t*)out_acc_buffer, PACKET_SIZE);
    // Removed delay(7) to allow full 44.1kHz throughput in Tone Mode
    delayMicroseconds(2000); 
  }
}
