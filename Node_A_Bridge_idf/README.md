# Node A: USB Audio Bridge (ESP32-S3)

## Overview
This directory contains the firmware for Node A, which runs on an ESP32-S3. Its primary role is to act as a USB Audio Class (UAC) device connected to the host PC and bridge that audio stream wirelessly to Node B over ESP-NOW.

## Development Information
- **Framework:** ESP-IDF (via PlatformIO)
- **Key Components:**
  - `managed_components/`: Contains external ESP-IDF components (like USB streaming libraries).
  - `src/`: Core logic for USB handling and ESP-NOW integration.
  - `sdkconfig.defaults`: ESP-IDF configurations tailored for ESP32-S3 with 16MB Flash / 8MB PSRAM (`esp32s3_n16r8`).

## Building & Modifying
- When editing ESP-IDF configurations, run `pio run -t menuconfig`.
- After changing any MAC address logic, ensure you run the root `auto_mac_setup.sh` to update `peers.h`.
- Build the project using `pio run` or upload it directly with `pio run -t upload`.
