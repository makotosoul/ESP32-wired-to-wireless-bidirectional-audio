void setup() {
  Serial.begin(115200);
  // Give Windows/Linux time to catch up
  for(int i=0; i<5; i++) {
    delay(1000);
    Serial.println("Starting up...");
  }
  Serial.println("========================================");
  Serial.println("ESP32-S3 N16R8 RECOVERY SUCCESSFUL");
  Serial.println("Hardware is alive and running!");
  Serial.println("========================================");
}

void loop() {
  Serial.println("ALIVE - Waiting for your instructions...");
  delay(1000);
}
