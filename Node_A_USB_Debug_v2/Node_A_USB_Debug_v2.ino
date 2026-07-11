// Flash (N16R8): hold BOOT, tap RST, release BOOT, then:
// arduino-cli compile --upload -p /dev/ttyACM0 \
//   --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,CDCOnBoot=default,USBMode=default \
//   ./Node_A_USB_Debug_v2

#include "USB.h"
#include "USBAudioCard.h"
#include <Adafruit_NeoPixel.h>

// WS2812 RGB: GPIO 48 on most ESP32-S3 DevKitC boards.
#define RGB_PIN   48
#define RGB_COUNT 1

Adafruit_NeoPixel rgb(RGB_COUNT, RGB_PIN, NEO_GRB + NEO_KHZ800);

USBAudioCard* AudioCard;
static volatile bool mic_stream_active = false;

static void setRGB(uint8_t r, uint8_t g, uint8_t b) {
    rgb.setPixelColor(0, rgb.Color(r, g, b));
    rgb.show();
}

static void onAudioEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (id == ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT) {
        arduino_usb_audio_card_event_data_t* ev = (arduino_usb_audio_card_event_data_t*)data;
        if (ev->interface_enable.interface == UAC_INTERFACE_MIC) {
            mic_stream_active = ev->interface_enable.enable;
            if (mic_stream_active) setRGB(0, 255, 0);
            else                   setRGB(0, 0, 0);
        }
    }
}

// Dedicated audio task — runs on Core 1, priority 15, paced by the RTOS tick.
//
// Why a task instead of loop():
//   loop() runs in the Arduino task which is preemptible by WiFi/system tasks.
//   That introduces multi-ms jitter that snd-usb-audio interprets as I/O error
//   on the isochronous IN endpoint. vTaskDelay is driven by the FreeRTOS tick
//   timer (hardware), giving sub-ms precision.
//
// We write unconditionally — even before the host opens the stream. This
// pre-fills TinyUSB's ~32ms software FIFO so the very first IN token after
// SET_INTERFACE alt=1 gets real data, not a zero-length packet.
//
// tud_audio_write returns the number of bytes actually accepted; when the
// FIFO is full it returns less than requested, which is fine — those bytes
// just get dropped, providing natural backpressure.
static void audioTask(void* pv) {
    static int sample_count = 0;
    static int16_t current_val = 5000;
    const TickType_t period = pdMS_TO_TICKS(1);
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        uint32_t rate = AudioCard->sampleRate();
        uint16_t samples_per_ms = (rate > 0) ? (uint16_t)(rate / 1000) : 48;
        if (samples_per_ms > 64) samples_per_ms = 64;

        int16_t buf[64];
        for (uint16_t i = 0; i < samples_per_ms; i++) {
            buf[i] = current_val;
            if (++sample_count >= 50) {
                current_val = -current_val;
                sample_count = 0;
            }
        }

        AudioCard->write(buf, samples_per_ms * sizeof(int16_t));

        // vTaskDelayUntil maintains a fixed period independent of how long
        // the write took — this is what gives us true 1ms cadence.
        vTaskDelayUntil(&wake, period);
    }
}

void setup() {
    rgb.begin();
    rgb.setBrightness(64);
    setRGB(0, 0, 0);

    for (int i = 0; i < 5; i++) {
        setRGB(255, 255, 255);
        delay(100);
        setRGB(0, 0, 0);
        delay(100);
    }

    AudioCard = new USBAudioCard(48000, UAC_BPS_16, UAC_SPK_NONE, UAC_MIC_MONO);
    AudioCard->onEvent(onAudioEvent);
    AudioCard->begin();
    USB.begin();

    // Core 1, priority 15: above WiFi (PRIO_MAX-2) and above loopTask (1).
    // Stack 4096 is plenty for a few hundred bytes of locals.
    xTaskCreatePinnedToCore(audioTask, "audio", 4096, NULL, 15, NULL, 1);
}

void loop() {
    // LED only: slow blue pulse when idle, solid green when streaming
    // (the green is set immediately in the event callback for snappiness).
    if (!mic_stream_active) {
        static uint32_t last_pulse_us = 0;
        static bool pulse_on = false;
        uint32_t now_us = micros();
        if (now_us - last_pulse_us >= 500000UL) {
            last_pulse_us = now_us;
            pulse_on = !pulse_on;
            setRGB(0, 0, pulse_on ? 255 : 0);
        }
    }
    delay(20);
}
