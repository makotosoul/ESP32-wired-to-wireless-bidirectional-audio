#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Wire.h>
#include "ESP_I2S.h" 
#include "freertos/ringbuf.h"
#include "../peers.h"

// --- Configuration ---
#define SAMPLE_RATE     16000
#define PACKET_SIZE     240 
#define RING_BUF_SIZE   8192 

#define I2C_SDA         33
#define I2C_SCL         32
#define ES8388_ADDR     0x10
#define I2S_BCK         27
#define I2S_WS          25
#define I2S_DO          26
#define I2S_MCLK        0
#define AMP_ENABLE_PIN  21

// --- Globals ---
uint8_t* peerAddress = nodeAAddress;
I2SClass i2s;
RingbufHandle_t audio_ring_buf;
volatile uint32_t recv_count = 0;
uint32_t last_print = 0;
bool byte_swap_enabled = true; // Most ES8388/ESP32 setups need this

void writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8388_ADDR);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}

void init_es8388() {
  Wire.begin(I2C_SDA, I2C_SCL);
  pinMode(AMP_ENABLE_PIN, OUTPUT);
  digitalWrite(AMP_ENABLE_PIN, HIGH);
  pinMode(19, OUTPUT); digitalWrite(19, HIGH); 
  delay(200);

  writeReg(0x00, 0x80); delay(50); 
  writeReg(0x00, 0x00); 
  writeReg(0x01, 0x30); 
  writeReg(0x02, 0x00); 
  writeReg(0x03, 0x00); 
  writeReg(0x04, 0x3c); 
  writeReg(0x08, 0x00); // Slave Mode

  // --- I2S FORMAT ---
  // 0x17: Bit 5:4 Format (00=I2S), Bit 3:1 Length (011=16-bit)
  // 0001 1000 = 0x18
  writeReg(0x17, 0x18); 
  
  writeReg(0x26, 0x00); 
  writeReg(0x27, 0x90); // LOUT1/ROUT1
  writeReg(0x2a, 0x00); 
  writeReg(0x2b, 0x22); // Volume
  writeReg(0x2c, 0x22); 
  
  Serial.println("[Node B] Codec Configured for 16-bit Standard I2S.");
}

void setup_i2s() {
  i2s.setPins(I2S_BCK, I2S_WS, I2S_DO, -1, I2S_MCLK);
  // Using 16-bit Stereo Standard I2S
  i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
}

void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
  if (len == PACKET_SIZE) {
    recv_count++;
    xRingbufferSend(audio_ring_buf, incomingData, len, 0);
  }
}

void TaskAudioPlayback(void *pvParameters) {
  size_t item_size;
  int16_t stereo_out[120 * 2]; 

  for (;;) {
    uint8_t *item = (uint8_t *)xRingbufferReceive(audio_ring_buf, &item_size, portMAX_DELAY);
    if (item != NULL) {
      int16_t* mono_ptr = (int16_t*)item;
      int num_samples = item_size / 2;

      for (int i = 0; i < num_samples; i++) {
        int16_t sample = mono_ptr[i];
        
        // --- THE BYTE SWAP ---
        if (byte_swap_enabled) {
          sample = (sample << 8) | ((sample >> 8) & 0x00FF);
        }

        stereo_out[i*2] = sample;
        stereo_out[i*2 + 1] = sample;
      }

      i2s.write((uint8_t*)stereo_out, num_samples * 4);
      vRingbufferReturnItem(audio_ring_buf, (void *)item);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[Node B] BIT-PROTOCOL DEBUG MODE");
  Serial.println("Type 's' to toggle Byte-Swapping.");

  audio_ring_buf = xRingbufferCreate(RING_BUF_SIZE, RINGBUF_TYPE_NOSPLIT);
  init_es8388();
  setup_i2s();

  WiFi.mode(WIFI_STA);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  if (esp_now_init() != ESP_OK) return;
  esp_now_register_recv_cb(OnDataRecv);

  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, peerAddress, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  xTaskCreatePinnedToCore(TaskAudioPlayback, "Playback", 8192, NULL, 15, NULL, 0);
}

void loop() {
  // Allow real-time debugging
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 's') {
      byte_swap_enabled = !byte_swap_enabled;
      Serial.printf("Byte Swap: %s\n", byte_swap_enabled ? "ON" : "OFF");
    }
  }

  if (millis() - last_print > 1000) {
    Serial.printf("[Node B] RX: %d packets/sec (Swap:%d)\n", recv_count, byte_swap_enabled);
    recv_count = 0;
    last_print = millis();
  }
}
