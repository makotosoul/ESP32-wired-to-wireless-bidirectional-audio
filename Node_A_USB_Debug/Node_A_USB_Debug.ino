#include "USB.h"
#include "USBAudioCard.h"

// 1. Declare as a pointer so it DOES NOT run before setup()
USBAudioCard* AudioCard; 

const int LED_PIN = 4; 

void setup() {
    pinMode(LED_PIN, OUTPUT);
    
    // 2. Startup sequence FIRST
    // You should now see this run successfully!
    for(int i = 0; i < 5; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(100);
        digitalWrite(LED_PIN, LOW);
        delay(100);
    }
    
    // 3. System is fully booted. Safe to create the Audio object now.
    AudioCard = new USBAudioCard(48000, UAC_BPS_16, UAC_SPK_NONE, UAC_MIC_MONO);
    
    // 4. Start the USB stack
    AudioCard->begin();
    USB.begin();
}

void loop() {
    static uint32_t last_blink = 0;
    static bool led_state = false;
    
    // Heartbeat
    if (millis() - last_blink > 500) {
        led_state = !led_state;
        digitalWrite(LED_PIN, led_state);
        last_blink = millis();
    }

    // Audio generation
    int16_t samples[48]; 
    static int sample_count = 0;
    static int16_t current_val = 5000;
    
    for (int i = 0; i < 48; i++) {
        samples[i] = current_val;
        sample_count++;
        if (sample_count >= 50) {
            current_val = -current_val; 
            sample_count = 0;
        }
    }
    
    // Note the arrow operator (->) for pointers
    AudioCard->write((uint8_t*)samples, sizeof(samples));
    
    yield();
}
