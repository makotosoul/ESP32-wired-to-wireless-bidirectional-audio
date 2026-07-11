# Wireless Audio Bitrate & Quality Analysis

## 1. The Goal: "YouTube Quality"
YouTube typically streams audio at a **44.1 kHz** or **48 kHz** sample rate with bitrates ranging from **128 kbps to 160 kbps** using lossy compression (Opus or AAC).

## 2. Current Problem: The "Muffled vs. Frizzy" Tradeoff

We are currently sending **Raw PCM (Uncompressed)** audio. The bandwidth requirement for mono raw audio is:
`Sample Rate × 16 bits = Bitrate`

### Scenario A: The "Muffled" Stream (Stable)
*   **Sample Rate:** 16.0 kHz or 24.0 kHz
*   **Bitrate:** 256 kbps - 384 kbps
*   **Result:** Stable audio, but sounds like a telephone (muffled). High frequencies (treble) are cut off.

### Scenario B: The "Frizzy" Stream (Unstable)
*   **Sample Rate:** 44.1 kHz or 48.0 kHz
*   **Bitrate:** 705 kbps - 768 kbps
*   **Result:** High-fidelity frequency response, but **ESP-NOW cannot handle the volume of data.**
*   **The "Frizz":** Packets are dropped because the wireless "pipe" is too narrow. This causes the grainy, frizzy distortion the user reported.

## 3. The Bottleneck: ESP-NOW Throughput
While the ESP32 is fast, **ESP-NOW** works best with a total throughput of under **500 kbps**.
*   **Raw 44.1 kHz (705 kbps)** is ~40% above the reliable limit.
*   **Raw 24.0 kHz (384 kbps)** is within the limit, but lacks treble.

## 4. The Solution: ADPCM Compression (IMA-ADPCM)
To achieve 44.1 kHz fidelity within the 500 kbps limit, we will implement **IMA-ADPCM compression**.

### How it works:
Instead of sending the full 16-bit value for every sample, ADPCM only sends the **difference** between samples using a 4-bit "step."

### The "YouTube Quality" Math:
*   **Original Audio:** 44.1 kHz @ 16-bit = **705 kbps** (Fails)
*   **ADPCM Audio:** 44.1 kHz @ 4-bit = **176 kbps** (Success!)

### Benefits:
1.  **Fidelity:** Full 22 kHz frequency response (Clear treble).
2.  **Stability:** 176 kbps is well below the ESP-NOW limit, so no packets will be dropped (No frizz).
3.  **Latency:** ADPCM is extremely fast to encode/decode, keeping the "Zero Latency" feel.

## 5. Implementation Roadmap
1.  **Node A (S3):** Implement the IMA-ADPCM encoder to compress 16-bit PCM to 4-bit nibbles.
2.  **Node B (A1S):** Implement the IMA-ADPCM decoder to expand 4-bit nibbles back to 16-bit PCM for the I2S hardware.
3.  **Packet Structure:** Each 240-byte packet will now contain **480 samples** of audio instead of 120.
