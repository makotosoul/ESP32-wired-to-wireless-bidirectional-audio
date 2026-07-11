/**
 * SIMPLE S3 BLINK TEST
 * Goal: Verify the board is taking code and running correctly.
 */

const int LED_PIN = 4; // Most S3 DevKits use Pin 4 for the user LED

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);
    Serial.println("S3 Blink Test Started!");
}

void loop() {
    digitalWrite(LED_PIN, HIGH);
    Serial.println("LED ON");
    delay(500);
    
    digitalWrite(LED_PIN, LOW);
    Serial.println("LED OFF");
    delay(500);
}
