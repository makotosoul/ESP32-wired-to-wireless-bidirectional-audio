# ESP32 Wired to Wireless Audio Bridge

## What is this project?
This project is a bidirectional wireless audio bridge utilizing ESP32 microcontrollers. It seamlessly connects an ESP32-S3 (acting as a standard USB audio device) to an ESP32-A1S Audio Kit over a custom wireless link.

## Developed By
This project is developed by **makotosoul**.

## Purpose
This project was created as the practical work for a bachelor's thesis in the ITBBA program at Haaga-Helia University of Applied Sciences. It provides a low-latency wireless audio extension, allowing a remote audio kit to function as a wireless headset without standard Bluetooth dependencies.

## Prerequisites
- **Hardware:** ESP32-S3 DevKit (Node A) and ESP32-A1S Audio Kit (Node B).
- **Software:** `esptool`, `arduino-cli`, `jq`, and PlatformIO (`pio`) installed in your system's PATH.

## Which script to run first?
First, you must run `./auto_mac_setup.sh`. This essential script scans for your connected boards, reads their MAC addresses, and injects them into the shared `peers.h` file so the nodes can establish a connection.

## How to use the scripts
1. Connect both ESP32 boards (Node A and Node B) to your computer via USB.
2. Run the `./auto_mac_setup.sh` script to configure the MAC addresses automatically.
3. Run the `./flash_bridge.sh` script to compile and flash the firmware to both Node A and Node B.
4. Once flashed, verify the audio connection on your host machine (e.g., using `arecord -l` and `aplay` on Linux) to confirm the devices are functioning.

## Developer Documentation
For additional details on modifying the firmware, ESP-IDF configuration, or hardware specifics for either node, please refer to their respective README files:
- [Node A (USB Audio Bridge)](./Node_A_Bridge_idf/README.md)
- [Node B (Hardware Audio Interface)](./Node_B_Bridge_idf/README.md)

## License
This project is licensed under the GNU Affero General Public License v3.0 (AGPLv3).
