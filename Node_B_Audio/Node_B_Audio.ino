/**
 * @brief We just set up the codec for a predefined board (AudioKitEs8388V1)
 * This follows the official library example for AI-Thinker boards.
 */
#include "AudioBoard.h"
#include "ESP_I2S.h"
#include <math.h>

using namespace audio_driver;

I2SClass i2s;

void setup() {
  // 1. Setup Logging as suggested in the README
  Serial.begin(115200);
  AudioDriverLogger.begin(Serial, AudioDriverLogLevel::Info); 

  // 2. Configure Codec using the predefined board object (AudioKitEs8388V1)
  // This object automatically knows the I2C (33/32) and Power (21/22/19) pins.
  CodecConfig cfg;
  cfg.input_device = ADC_INPUT_LINE1;
  cfg.output_device = DAC_OUTPUT_ALL;
  cfg.i2s.bits = BIT_LENGTH_16BITS;
  cfg.i2s.rate = RATE_16K; // Set to 16kHz to match our project
  
  Serial.println("Starting AI-Thinker AudioKit (V1/V2.2)...");
  if (AudioKitEs8388V1.begin(cfg)) {
    Serial.println("Board Initialized: SUCCESS");
  } else {
    Serial.println("Board Initialized: FAILED");
  }

  // 3. Set Master Volume
  AudioKitEs8388V1.setVolume(85);

  // 4. Setup I2S Data
  // We fetch the pins directly from the board definition to be 100% accurate.
  auto i2s_pins = AudioKitEs8388V1.pins().getI2SPins(PinFunction::CODEC);
  if (i2s_pins) {
    auto p = i2s_pins.value();
    Serial.printf("I2S Pins: BCK:%d, WS:%d, DO:%d, MCLK:%d\n", p.bck, p.ws, p.data_out, p.mclk);
    i2s.setPins(p.bck, p.ws, p.data_out, p.data_in, p.mclk);
  }
  
  i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);

  Serial.println("\n--- HARDWARE TEST READY ---");
  Serial.println("Hold KEY 1 (usually GPIO 36) to hear a local buzz.");
}

void loop() {
  // Use the library's official key checking method
  if (AudioKitEs8388V1.isKeyPressed(1)) {
    int16_t samples[256];
    for (int i = 0; i < 128; i++) {
      // 100Hz Square wave (LOUD)
      int16_t val = (i % 80 < 40) ? 10000 : -10000;
      samples[i*2] = val;
      samples[i*2+1] = val;
    }
    i2s.write((uint8_t*)samples, sizeof(samples));
    
    if (millis() % 250 < 20) {
      Serial.println("BEEPING...");
    }
  }
}
