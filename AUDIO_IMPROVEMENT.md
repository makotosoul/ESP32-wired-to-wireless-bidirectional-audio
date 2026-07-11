# Wireless speaker audio improvement

Guide for the **host → Node A → ESP-NOW → Node B → A1S speaker** path. Bluetooth quality on the host is out of scope here.

Related docs:

- [`bridge_spk.h`](bridge_spk.h) — shared speaker wire format and Opus settings
- [`Node_A_Bridge_idf/BRIDGE_JOURNEY.md`](Node_A_Bridge_idf/BRIDGE_JOURNEY.md) — full bridge (Node A)
- [`Node_B_Bridge_idf/BRIDGE_B_IDF_JOURNEY.md`](Node_B_Bridge_idf/BRIDGE_B_IDF_JOURNEY.md) — Node B ESP-IDF port
- [`audio_bitrate_analysis.md`](audio_bitrate_analysis.md) — ESP-NOW throughput vs sample rate (older ADPCM-era notes)

---

## 1. Problem statement

Speaker audio is **lossy** over ESP-NOW because:

- ESP-NOW payload is capped at **250 bytes** per packet.
- **Lossless** PCM at 48 kHz mono (~768 kb/s) cannot fit the radio budget.
- Delivery can be healthy (`played ≈ recv`, `dropthru: 0`) while **codec quality** still limits how good it sounds.

The mic path is separate: **200 B IMA ADPCM @ 16 kHz** (unchanged by speaker Opus work).

---

## 2. Codec history

| Stage | Wire format | Notes |
|-------|-------------|--------|
| **IMA ADPCM (legacy)** | Fixed **240 B** / 10 ms @ 48 kHz mono | Simple; grain, dull highs, pumping. Node B still accepts for rollback. |
| **Opus v2 (current)** | Variable **≤ 250 B**; header `0x4F` + `opus_len` + payload | Perceptual codec; better quality per bit. Frame-independent drops. |

Detection helpers live in [`bridge_spk.h`](bridge_spk.h): `bridge_spk_is_opus_pkt()`, `bridge_spk_is_adpcm_pkt()`.

**Memory gotchas (already addressed in firmware):**

- **Node A (ESP32-S3 N16R8):** Opus encode needs **OPI PSRAM** in `Node_A_Bridge_idf/sdkconfig.defaults`.
- **Node B (ESP32-WROOM, no PSRAM):** Opus decode uses **60 KB** non-threadsafe pseudostack + internal-only RAM in `Node_B_Bridge_idf/sdkconfig.defaults`. Boot must show `Opus warmup OK`.

---

## 3. Current defaults

| Parameter | Value |
|-----------|--------|
| Sample rate | **48 kHz** mono |
| Frame | **10 ms** = **480** samples |
| Opus bitrate | **128000** b/s (`BRIDGE_SPK_OPUS_BITRATE`) |
| Encoder complexity | **4** (`BRIDGE_SPK_OPUS_COMPLEXITY_ENC`) |
| Decoder complexity | **2** (`BRIDGE_SPK_OPUS_COMPLEXITY_DEC`) |
| Max packet | **250 B** (248 B max Opus payload after 2 B header) |
| Mic (unchanged) | **200 B** ADPCM @ **16 kHz**, 25 ms |

Only **Node A** applies bitrate via `OPUS_SET_BITRATE` in `spk_opus_encoder_init()`. Node B decodes whatever bitstream arrives.

---

## 4. Bitrate upgrade procedure (128 kb/s)

### 4.1 Code change

In [`bridge_spk.h`](bridge_spk.h):

```c
#define BRIDGE_SPK_OPUS_BITRATE     128000
```

Rebuild **both** nodes so they share the same header revision (bitrate affects encode only, but keep A/B builds paired).

### 4.2 Build

```bash
cd /home/makotosoul/project/Thesis/Node_A_Bridge_idf
pio run

cd /home/makotosoul/project/Thesis/Node_B_Bridge_idf
pio run
```

### 4.3 Flash

```bash
cd /home/makotosoul/project/Thesis
./flash_bridge.sh
```

If Node A’s S3 serial port is missing: hold **BOOT**, tap **RST**, release **BOOT**, re-run.

Or flash individually (replace ports from `arduino-cli board list`):

```bash
pio run -d /home/makotosoul/project/Thesis/Node_A_Bridge_idf -t upload --upload-port /dev/ttyACM0
pio run -d /home/makotosoul/project/Thesis/Node_B_Bridge_idf -t upload --upload-port /dev/ttyUSB0
```

### 4.4 Boot log checks

**Node A (S3 serial):**

- `Opus speaker encoder: 48000 Hz mono, 128000 b/s, complexity 4`
- `Opus warmup OK (N B frame)` — expect **N ~ 120–200** for silence warmup at 128 kb/s
- No red LED / no `Opus speaker encoder init failed`

**Node B (A1S serial):**

- `heap SPIRAM free=0` (normal on WROOM)
- `Opus warmup OK`
- No reboot when host playback starts

Monitor Node B:

```bash
pio device monitor -d /home/makotosoul/project/Thesis/Node_B_Bridge_idf -b 115200 -p /dev/ttyUSB0
```

### 4.5 Packet size note

At 128 kb/s, average Opus payload is ~**160 B**/frame; peaks can be higher. If Node A logs frequent `opus_encode` warnings, frames may be hitting the **248 B** payload cap — try **96000** b/s or enable stricter VBR (see §7).

---

## 5. Verification checklist

### Serial stats (Node B, ~1/s)

Healthy during playback:

```text
[Spk] recv:~100 played:~100 opus_ok:~100 opus_fail:0 dropthru:0 isr_drop:0 qdepth:0
[Mic] Sent:~40 Peak>0
```

**Red flags:** `opus_fail` > 0, `dropthru` or `isr_drop` > 0, `played` << `recv`, mic `Sent` drops during heavy speaker use.

### Listen test

1. **Speaker only:** Play **48 kHz mono** to the USB UAC device; listen on the A1S kit speaker. Compare clarity vs previous 80 kb/s (less grain, better highs — may be subtle on the kit speaker).
2. **Duplex:** Play to speaker while recording the ESP mic on the host; confirm mic level and speaker both work.

### Linux quick test (from [`flash_bridge.sh`](flash_bridge.sh))

```bash
arecord -l
arecord -D plughw:CARD=Device,DEV=0 -f S16_LE -r 48000 -c 1 -d 5 /tmp/mic.wav
aplay /tmp/mic.wav
aplay -D plughw:CARD=Device,DEV=0 -f S16_LE -r 48000 -c 1 /path/to/test.wav
```

---

## 6. Troubleshooting

| Symptom | Likely cause | Action |
|---------|----------------|--------|
| Node A reboot on playback | Opus OOM without PSRAM | Confirm `CONFIG_SPIRAM=y` in Node A `sdkconfig.defaults`; delete stale `sdkconfig.esp32s3_n16r8` and rebuild |
| Node B reboot / static on playback | Opus OOM on WROOM | Confirm Node B `sdkconfig.defaults` (60 KB pseudostack, internal-only); boot must pass warmup |
| `opus_fail` rising | Corrupt/truncated packets or version skew | Flash **both** nodes; check `opus_encode` warnings on Node A |
| `opus_encode` warnings | Payload > 248 B | Lower bitrate (96k/80k) or constrain VBR |
| Quality “same as ADPCM” | Kit speaker masking; 80k already decent | Try 128k (this doc); external speaker; §7 levers |
| Need legacy path | Node B cannot run Opus | Revert Node A speaker to **240 B ADPCM** per [`BRIDGE_JOURNEY.md`](Node_A_Bridge_idf/BRIDGE_JOURNEY.md) |

### Rollback bitrate

1. Set `BRIDGE_SPK_OPUS_BITRATE` to **80000** in `bridge_spk.h`.
2. Rebuild and flash **both** nodes.

---

## 7. Future levers (not implemented)

| Lever | Tradeoff |
|-------|----------|
| **96 kb/s** | Middle ground if 128k causes encode size or CPU issues |
| `OPUS_SET_VBR_CONSTRAINT(1)` | Smaller peak packet size; may reduce spikes over 248 B |
| Encoder **complexity 5–6** | Better quality per bit; more CPU on S3 |
| `SPK_PCM_GAIN_SHIFT` (Node B) | Louder playback; does not fix codec artifacts |
| Better physical speaker | Often the limiter after codec is “good enough” |
| Longer frame (20 ms) | Fewer packets/s; different latency/queue behavior — needs wire-format change |

Throughput background: [`audio_bitrate_analysis.md`](audio_bitrate_analysis.md).

---

## 8. Quality ceiling and conclusions

After ADPCM → Opus, stable ESP-NOW delivery (`played ≈ recv`, `dropthru: 0`, `opus_fail: 0`), and a bitrate step **80 → 128 kb/s**, further **firmware-only** speaker tuning on this hardware is expected to yield **little or no audible gain**. That matches listening tests where 128 kb/s sounded similar to 80 kb/s and to the earlier ADPCM path: the bridge is doing its job; perception is limited elsewhere.

### What was optimized (software)

| Layer | Status |
|-------|--------|
| Transport | ESP-NOW channel 1; ~100 speaker frames/s; queue healthy |
| Codec | Opus v2 @ 48 kHz mono, **128 kb/s**, enc complexity 4 |
| Node A | S3 + PSRAM; encoder warmup at boot |
| Node B | WROOM, no PSRAM; 60 KB Opus pseudostack; `Opus warmup OK` at boot |
| Duplex | Mic path unchanged (16 kHz ADPCM); speaker work did not break mic send |

Example of a **healthy Node B boot** (decode path ready before playback):

```text
I bridge_b: heap SPIRAM free=0 internal free=206408
I bridge_b: Opus speaker decoder: 48000 Hz mono
I bridge_b: Opus warmup OK (3 B frame)
I bridge_b: Tasks running. spk Core 1 (prio 12), mic Core 0 (prio 8).
```

### Dominant limits (why “no more improvement” is reasonable)

1. **ESP-NOW payload (~250 B)** — Caps how much information each 10 ms frame can carry. Lossless 48 kHz mono is not viable on this link; Opus is already a strong fit for the budget.
2. **ESP32 + Opus on WROOM** — Decode memory and CPU are tuned for stability (non-threadsafe pseudostack, internal RAM). There is little headroom for heavier codecs or much higher bitrates without reliability risk.
3. **Playback hardware (A1S kit)** — ES8388 → on-board speaker/enclosure often masks codec differences once wireless delivery is clean. This is a **transducer** limit, not an ES8388 “broken codec” issue.
4. **Mic path (separate)** — Host mic quality remains **16 kHz ADPCM**; end-to-end “system” fidelity is not full-band even when the speaker path is maxed out.

### What would be required for a large audible jump

| Change | Why it matters |
|--------|----------------|
| Better speaker / headphones / line-out on Node B | Biggest practical gain after wireless is stable |
| Different link (e.g. Wi-Fi with larger MTU, wired audio) | Removes the 250 B-per-packet ceiling |
| Lossless or much higher bitrate on wire | Needs more airtime than ESP-NOW usefully provides at 48 kHz |
| Mic upgrade (48 kHz + stronger codec on mic packets) | Improves **capture** only; independent of speaker Opus |

### Thesis-ready summary

> Wireless speaker quality on this bridge is dominated by **lossy compression under an ESP-NOW byte budget** and by **kit playback hardware**, not by missed firmware optimizations. The project progressed from IMA ADPCM to Opus at 128 kb/s with verified delivery and stable duplex; additional bitrate or complexity tweaks are diminishing returns unless the radio format or physical output path changes.

§7 lists minor experimental levers; treat them as optional, not expected wins.

---

## 9. Changelog

| Date | Change |
|------|--------|
| May 2026 | Opus speaker path stable on S3 + WROOM; bitrate raised **80 → 128 kb/s** |
| May 2026 | Added §8 quality ceiling / conclusions after hardware listening tests |
