#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "USB.h"
#include "USBAudioCard.h"
#include "../peers.h"

#define SAMPLE_RATE     16000
#define PACKET_SIZE     240 

uint8_t* peerAddress = nodeBAddress;
// Force 16kHz, 16-bit, MONO
USBAudioCard AudioCard(SAMPLE_RATE, UAC_BPS_16, UAC_SPK_MONO, UAC_MIC_NONE);

uint8_t accumulation_buffer[PACKET_SIZE];
size_t accumulation_index = 0;
uint32_t packets_sent = 0;
uint32_t last_report_ms = 0;

void onSpeakerData(void *data, uint16_t len) {
  uint8_t* incoming_ptr = (uint8_t*)data;
  for (size_t i = 0; i < len; i++) {
    accumulation_buffer[accumulation_index++] = incoming_ptr[i];
    if (accumulation_index >= PACKET_SIZE) {
      esp_now_send(peerAddress, accumulation_buffer, PACKET_SIZE);
      accumulation_index = 0;
      packets_sent++;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  AudioCard.onData(onSpeakerData);
  AudioCard.begin();
  USB.begin();

  WiFi.mode(WIFI_STA);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) return;

  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, peerAddress, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  Serial.println("[Node A] Standard Mono Bridge Ready.");
}

void loop() {
  if (millis() - last_report_ms > 1000) {
    Serial.printf("[Node A] Streaming: %d packets/sec\n", packets_sent);
    packets_sent = 0;
    last_report_ms = millis();
  }
}
