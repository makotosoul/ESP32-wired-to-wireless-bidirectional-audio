/**
 * Master Input Scanner for Ai-Thinker ESP32 Audio Kit (ES8388)
 * Designed for hardware debugging of 3.5mm jack and ADC channel mapping.
 */

#define AUDIOKIT_BOARD 5
#include "AudioTools.h"
#include "AudioBoard.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"

// Instantiate the kit using the ES8388 driver
AudioBoardStream kit(AudioKitEs8388V1);

// Array of all available ES8388 analog inputs
input_device_t inputs[] = {
  ADC_INPUT_LINE1, 
  ADC_INPUT_LINE2, 
  ADC_INPUT_ALL, 
  ADC_INPUT_DIFFERENCE
};

String inputNames[] = {
  "ADC_INPUT_LINE1", 
  "ADC_INPUT_LINE2", 
  "ADC_INPUT_ALL", 
  "ADC_INPUT_DIFFERENCE"
};

const int numInputs = 4;
int currentInputIdx = 0;

// Default configuration for reception
auto cfg = kit.defaultConfig(RX_MODE);

// Timing and Peak tracking variables
unsigned long lastSwitchTime = 0;
unsigned long lastPrintTime = 0;
int16_t peakL = 0;
int16_t peakR = 0;

void setup() {
  Serial.begin(115200);
  while(!Serial); // Wait for Serial Monitor
  Serial.println("\n--- ES8388 Master Input Scanner ---");
  Serial.println("Cycling through ADC channels every 8 seconds...");

  // Setup I2S and Codec configuration
  cfg.input_device = inputs[currentInputIdx];
  cfg.sample_rate = 44100;
  cfg.channels = 2;
  cfg.bits_per_sample = 16;
  
  // Start the hardware
  kit.begin(cfg);
  
  // Set initial high gain for easier debugging
  ::es8388_set_mic_gain(audio_driver::MIC_GAIN_24DB);
  
  lastSwitchTime = millis();
  lastPrintTime = millis();
  
  Serial.print("Initial Channel: ");
  Serial.println(inputNames[currentInputIdx]);
}

void loop() {
  // Constantly read the I2S audio buffer from the kit
  uint8_t buffer[512];
  size_t bytesRead = kit.readBytes(buffer, 512);
  
  if (bytesRead > 0) {
    int16_t* samples = (int16_t*)buffer;
    int numSamples = bytesRead / 2; // 2 bytes per 16-bit sample

    // Calculate absolute peak values for the Left and Right channels
    for (int i = 0; i < numSamples; i += 2) {
      int16_t left = abs(samples[i]);
      int16_t right = (i + 1 < numSamples) ? abs(samples[i+1]) : 0;
      
      if (left > peakL) peakL = left;
      if (right > peakR) peakR = right;
    }
  }

  unsigned long now = millis();

  // Print the peaks to Serial Monitor every 500ms
  if (now - lastPrintTime >= 500) {
    Serial.print("[");
    Serial.print(inputNames[currentInputIdx]);
    Serial.print("] Peaks -> L: ");
    Serial.print(peakL);
    Serial.print(" | R: ");
    Serial.println(peakR);
    
    // Reset peaks for the next 500ms window
    peakL = 0;
    peakR = 0;
    lastPrintTime = now;
  }

  // Cycle to the next input in the array every 8 seconds
  if (now - lastSwitchTime >= 8000) {
    currentInputIdx = (currentInputIdx + 1) % numInputs;
    
    Serial.println("\n------------------------------------------------");
    Serial.print(">>> SWITCHING TO: ");
    Serial.println(inputNames[currentInputIdx]);
    Serial.println("------------------------------------------------\n");
    
    // Perform a safe restart of the kit to apply the new ADC channel
    kit.end();
    cfg.input_device = inputs[currentInputIdx];
    kit.begin(cfg);
    
    // Re-apply gain settings after driver re-initialization
    ::es8388_set_mic_gain(audio_driver::MIC_GAIN_24DB);
    
    lastSwitchTime = now;
    // Reset peaks on switch to avoid carrying over noise from previous channel
    peakL = 0;
    peakR = 0;
  }
}
