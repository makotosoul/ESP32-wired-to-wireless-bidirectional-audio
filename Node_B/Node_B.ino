#include <esp_now.h>
#include <WiFi.h>
#include "../peers.h"

// Node B's peer is Node A
uint8_t* peerAddress = nodeAAddress;

// --- Callbacks ---
void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.print("\r\nLast Packet Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
}

void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
  char message[len + 1];
  memcpy(message, incomingData, len);
  message[len] = '\0';
  Serial.print("\r\n[Node A]: ");
  Serial.println(message);
  Serial.print("[Node B]: "); // Re-print prompt
}

// --- FreeRTOS Task ---
void TaskSerialRead(void *pvParameters) {
  (void) pvParameters;
  
  for (;;) {
    if (Serial.available()) {
      String msg = Serial.readStringUntil('\n');
      msg.trim();
      
      if (msg.length() > 0) {
        esp_err_t result = esp_now_send(peerAddress, (uint8_t *) msg.c_str(), msg.length());
        
        if (result != ESP_OK) {
          Serial.println("Error sending the data");
        }
      }
      Serial.print("[Node B]: ");
    }
    vTaskDelay(10 / portTICK_PERIOD_MS); // Small delay to prevent watchdog issues
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  // Register peer
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo)); // Zero-initialize structure
  memcpy(peerInfo.peer_addr, peerAddress, 6);
  peerInfo.channel = 0;  
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA; // Explicitly set interface
  
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }

  // Create the Serial Read Task
  xTaskCreatePinnedToCore(
    TaskSerialRead,
    "SerialRead",
    4096,  // Stack size
    NULL,
    1,     // Priority
    NULL,
    1      // Core 1
  );

  Serial.println("Node B Initialized. Type your message below:");
  Serial.print("[Node B]: ");
}

void loop() {
  // FreeRTOS handles the logic in TaskSerialRead
  vTaskDelete(NULL); 
}
