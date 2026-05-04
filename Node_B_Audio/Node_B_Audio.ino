#include "AudioBoard.h"
#include "ESP_I2S.h"
#include <esp_now.h>
#include <WiFi.h>
#include "../peers.h"

using namespace audio_driver;

I2SClass i2s;

// --- ESP-NOW RECEIVE (Audio from Node A) ---
void onDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  int16_t mono_samples[120];
  int16_t stereo_samples[240];
  memcpy(mono_samples, incomingData, len);
  
  for (int i = 0; i < 120; i++) {
    stereo_samples[i*2] = mono_samples[i];
    stereo_samples[i*2+1] = mono_samples[i];
  }
  i2s.write((uint8_t*)stereo_samples, sizeof(stereo_samples));
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // 1. Initialize Codec for Headphones only
  CodecConfig cfg;
  cfg.input_device = ADC_INPUT_NONE; 
  cfg.output_device = DAC_OUTPUT_ALL;
  cfg.i2s.bits = BIT_LENGTH_16BITS;
  cfg.i2s.rate = RATE_16K;
  
  if (AudioKitEs8388V1.begin(cfg)) {
    Serial.println("Codec: SUCCESS");
  }
  AudioKitEs8388V1.setVolume(85);

  // 2. Setup I2S Output (DI set to -1)
  i2s.setPins(27, 25, 26, -1, 0);
  i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);

  // 3. Setup ESP-NOW
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) return;
  esp_now_register_recv_cb(esp_now_recv_cb_t(onDataRecv));

  Serial.println("[Node B] One-Way Stream Sink Ready.");
}

void loop() {
  // Listen only mode
  delay(100);
}
