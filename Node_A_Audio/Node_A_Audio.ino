#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "USB.h"
#include "USBAudioCard.h"
#include "../peers.h"

#define SAMPLE_RATE     16000
#define PACKET_SIZE     240    
#define TONE_MODE_PIN   0      

uint8_t* peerAddress = nodeBAddress;
USBAudioCard AudioCard(SAMPLE_RATE, UAC_BPS_16, UAC_SPK_MONO, UAC_MIC_NONE);

int16_t accumulation_buffer[PACKET_SIZE / 2]; // Work in 16-bit samples
size_t accumulation_index = 0;
uint32_t packets_sent = 0;
uint32_t last_report_ms = 0;

float phase = 0;
float phase_inc = 2.0 * PI * 440.0 / SAMPLE_RATE;

void onSpeakerData(void *data, uint16_t len) {
  if (digitalRead(TONE_MODE_PIN) == LOW) return;

  // We know UAC is 16-bit, so we cast to int16_t
  int16_t* incoming_samples = (int16_t*)data;
  size_t num_samples = len / 2;

  for (size_t i = 0; i < num_samples; i++) {
    int16_t s = incoming_samples[i];
    
    // --- OPTIONAL: Swapping here is often cleaner ---
    // s = (s << 8) | ((s >> 8) & 0x00FF); 
    
    accumulation_buffer[accumulation_index++] = s;
    
    if (accumulation_index >= (PACKET_SIZE / 2)) {
      esp_now_send(peerAddress, (uint8_t*)accumulation_buffer, PACKET_SIZE);
      accumulation_index = 0;
      packets_sent++;
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
  memcpy(peerInfo.peer_addr, peerAddress, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  Serial.println("[Node A] SAMPLE-SAFE 16kHz Ready.");
}

void loop() {
  if (digitalRead(TONE_MODE_PIN) == LOW) {
    for (int i = 0; i < (PACKET_SIZE / 2); i++) {
      accumulation_buffer[i] = (int16_t)(10000.0 * sin(phase));
      phase += phase_inc;
      if (phase > 2.0 * PI) phase -= 2.0 * PI;
    }
    esp_now_send(peerAddress, (uint8_t*)accumulation_buffer, PACKET_SIZE);
    packets_sent++;
    delay(7); 
  }

  if (millis() - last_report_ms > 1000) {
    Serial.printf("[Node A] PPS: %d\n", packets_sent);
    packets_sent = 0;
    last_report_ms = millis();
  }
}
