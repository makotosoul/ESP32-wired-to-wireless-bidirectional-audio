# Node B: Hardware Audio Interface (ESP32-A1S)

## Overview
This directory contains the firmware for Node B, targeting the ESP32-A1S Audio Kit. It receives wireless audio from Node A via ESP-NOW, decodes it, and outputs it through the onboard audio codec (and vice-versa for the microphone).

## Development Information
- **Framework:** ESP-IDF (via PlatformIO)
- **Key Components:**
  - `src/`: Contains I2S codec initialization, audio buffering, and ESP-NOW transceiver logic.
  - `sdkconfig.defaults`: Custom configurations for the A1S board.

## Building & Modifying
- This node relies heavily on the specific pinouts of the A1S Audio Kit (I2S, I2C). Be careful when modifying GPIO mappings in the code.
- To change ESP-IDF parameters, use `pio run -t menuconfig`.
- Build and upload with `pio run -t upload`. For debugging codec issues, use the serial monitor: `pio device monitor -b 115200`.

## Known Limitations
The ESP-NOW link is unencrypted and does not validate the sender's MAC address. `peers.h` contains hardware MAC addresses and must not be committed publicly. See [Known Limitations](../README.md#known-limitations) in the root README.
