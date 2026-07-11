# Project Instructions - Thesis

## Hardware & Architecture
- **ESP32-S3 (Node A / Dongle):**
  - **USB Audio:** Must be compiled with `USBMode=tinusb` to act as a USB Microphone/Speaker.
  - **Task Affinity:** To avoid resource conflicts with the WiFi/ESP-NOW stack, always pin high-frequency audio/USB tasks to **Core 1**. Running them on Core 0 will lead to dropped ESP-NOW packets.
- **Audio Configuration:**
  - Standardized on **16kHz Mono** for ESP-NOW transmission.
  - ADPCM compression is used, packing two 4-bit samples per byte.
  - Packet Size: 200 bytes (contains 400 samples / 25ms of audio).
