# Node B Bridge — Arduino → ESP-IDF Migration Journey

This document captures why we ported the Node B audio kit side of the bridge
from the Arduino I2S library to a native ESP-IDF (PlatformIO) project, and
the design decisions behind the new code.

## TL;DR

The Arduino sketch `Node_B_Bridge/` was capped at **~41 speaker frames/sec**
out of a required ~100, regardless of how aggressively we trimmed mic
contention. The Arduino `I2SClass` serialises TX and RX internally; once the
mic loop touched I2S, the speaker loop was throttled to the mic's pace. This
ESP-IDF port uses the `i2s_std` driver directly with **separate TX and RX
channel handles**, giving the speaker its own DMA queue, no shared mutex, and
the headroom to hit the full DAC rate.

**Status (May 2026):** **Production-ready on hardware.** Full-duplex bridge
verified: host speaker → Node A → ESP-NOW → Node B DAC at ~100 `played`/s,
and mic capture → ESP-NOW → Node A UAC **at the same time** (`Sent` ~40/s,
`[Mic] Peak` non-zero while `[Spk] recv` ~100/s). Post-port debug covered
I2C `bus_handle`, codec register writes, speaker gain chain, and LINE2 mic
routing.

## Symptoms before migration

```
[Mic] Peak:18 Sent:41 Fails:0 | [Spk] recv:102 played:41 dropthru:61 isr_drop:0 qdepth:9
```

- ESP-NOW link healthy: `recv ≈ 102/s` (Node A sending the full 100/s).
- ISR queue healthy: `isr_drop = 0` (so ADPCM was at least staying in sync
  for the packets we did play, thanks to the decode-through fix).
- Playback at the DAC: only **`played: 41/s`**. The other 60+ packets per
  second were being decode-through'd (correctly advancing `dec_spk`, but
  dumping the audio). That's why playback sounded like "small snippets of the
  song through static" — only ~40% of the waveform was reaching the speaker.
- `played` matched `Sent` exactly, the signature of TX/RX serialisation
  inside `I2SClass`.

## What we tried (and why it didn't work)

| Attempt | Result |
| --- | --- |
| Remove `vTaskDelayUntil(0)` pacing assert | Fixed the boot loop, no throughput change |
| Decode-through dropped packets to keep `dec_spk` aligned | Killed the catastrophic static; still only 41 played/s |
| Deepen `SPEAKER_QUEUE_DEPTH` to 64 | No effect — queue wasn't the bottleneck |
| Chunk mic `readBytes` into 5 × 5 ms pieces, release `i2sMutex` between | No effect — `I2SClass` still serialised the underlying I2S peripheral |

Each step proved a different "obvious" hypothesis wasn't the cause. After the
chunking failed, the diagnostic counters confirmed the bottleneck was inside
`i2s.write()` itself, and the equality of `played` and `Sent` pointed
squarely at TX/RX serialisation in the Arduino wrapper.

## Why ESP-IDF fixes it

`Arduino-ESP32`'s `I2SClass` is a thin wrapper over the same `i2s_std` driver
we use here, but it:

1. Allocates a single I2S channel and reuses it for both read and write
   (depending on the call), instead of separate `tx_chan` + `rx_chan`.
2. Provides no way to size the DMA descriptor count / frame count.
3. Forces the application to add its own mutex on top, even though the
   underlying driver supports concurrent calls on separate handles.

In ESP-IDF you allocate full-duplex like this:

```c
i2s_chan_config_t cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
cfg.dma_desc_num  = 8;
cfg.dma_frame_num = 240;          // 5 ms @ 48 kHz stereo per descriptor
i2s_new_channel(&cfg, &tx_chan, &rx_chan);   // both handles, one call
```

`tx_chan` and `rx_chan` have independent DMA queues. `i2s_channel_write` on
TX blocks only on TX DMA having room; `i2s_channel_read` on RX blocks only
on RX DMA filling. They don't contend at all. No mutex needed in the app.

## Architecture overview

```
                            ┌──────────── Core 0 ────────────┐
                            │  WiFi + ESP-NOW                │
                            │  on_now_recv → xQueueSend      │
                            │  mic_task (prio 8):            │
                            │    i2s_channel_read (rx_chan)  │
                            │    downsample + ADPCM encode   │
                            │    esp_now_send → Node A       │
                            └────────────────────────────────┘

                            ┌──────────── Core 1 ────────────┐
                            │  speaker_task (prio 12):       │
                            │    xQueueReceive (s_spk_queue) │
                            │    decode-through drops to     │
                            │      keep dec_spk in sync      │
                            │    ADPCM decode (480 samples)  │
                            │    duplicate mono → stereo     │
                            │    i2s_channel_write (tx_chan) │
                            └────────────────────────────────┘

Hardware (A1S V2.2 ES8388):
  I2S: BCLK=27  LRCK=25  DOUT=26 (DSDIN)  DIN=35 (ASDOUT)  MCLK=0
  I2C: SDA=33   SCL=32   addr=0x10 (7-bit)
  PA enable: GPIO 21
```

## Key code modules

- **Mic ADPCM** — Bit-identical to Node A's mic decoder. 4 bits/sample, 200 B / 25 ms.
- **Speaker Opus** — Node A sends v2 packets (`bridge_spk.h`); default **128 kb/s**
  mono @ 48 kHz (see [`AUDIO_IMPROVEMENT.md`](../AUDIO_IMPROVEMENT.md)). Node B decodes
  with `micro-opus`. Legacy **240 B ADPCM** still accepted for rollback.
- **ESP32-WROOM (no PSRAM)** — A1S kits usually have **no external RAM**. Opus must use
  [`sdkconfig.defaults`](sdkconfig.defaults): `NONTHREADSAFE` pseudostack, **60 KB**,
  internal-only allocations. Decoder init + warmup run in `app_main` before tasks; boot
  log must show `Opus warmup OK`. If init fails or playback still reboots, revert Node A
  speaker to ADPCM (see [`BRIDGE_JOURNEY.md`](../Node_A_Bridge_idf/BRIDGE_JOURNEY.md)).
- **`wifi_espnow_init`** — STA mode, channel 1, no power save, one
  pre-registered peer (Node A's MAC from `peers.h`).
- **`i2s_init`** — Full-duplex `i2s_new_channel` + standard mode init on
  both handles. DMA: 8 × 240 frames = 40 ms buffering per direction.
- **`i2c_bus_init`** — `i2c_new_master_bus()` (IDF 5.3+ master driver). The
  handle is passed to `audio_codec_new_i2c_ctrl` as `bus_handle` (required;
  see debug journey below).
- **`codec_init`** — `espressif/esp_codec_dev` opens the ES8388 and enables
  the PA (GPIO 21). Register tweaks use **`s_codec_ctrl->write_reg()`** (the
  ES8388 driver has no `set_reg`, so `esp_codec_dev_write_reg()` is a no-op).
  After open: **LINE2** on `ADCCONTROL2` (`0x50`), **LOUT/ROUT +3 dB** on
  `DACCONTROL24/25` (`0x21`), `esp_codec_dev_set_out_vol(..., 100)`,
  `esp_codec_dev_set_in_gain(..., 6 dB)` (`MIC_IN_GAIN_DB`). PCM I/O is **not** via
  `esp_codec_dev_read/write` — tasks call `i2s_channel_read/write` directly;
  `esp_codec_dev_open` still enables both I2S channels through the data IF.
- **`speaker_task`** — Variable-length queue items (≤ 250 B): Opus v2 decode
  (480 samples) or legacy ADPCM. `SPK_PCM_GAIN_SHIFT`, stereo dup, I2S write.
  On queue trim, ADPCM drops decode-through; Opus drops are stateless.
- **`mic_task`** — One 25 ms read (1200 stereo pairs), pick L every 6
  source samples → 16 kHz mono, ADPCM encode, send 200 B packet. The
  chunked-read scaffolding from the Arduino sketch is gone — the read no
  longer blocks the speaker, so chunking is unnecessary.

## Why `mic_task` is on Core 0

Mic packet rate is only **40/sec** (one 25 ms read per packet) and the
speaker path needs every CPU cycle on Core 1 it can get. Co-locating
`mic_task` with WiFi on Core 0 keeps `esp_now_send` cheap (the call doesn't
have to cross cores into the WiFi task). The mic encode is light enough
(800 ADPCM ops / packet) that it doesn't visibly hurt WiFi.

## Why we kept the `decode-through on drop` logic

The deeper queue (64 packets) makes ISR-side drops vanish, but the queue
trim loop in `speaker_task` is still needed: under sustained backpressure
(e.g. a transient WiFi stall) the queue can creep above the latency cap.
Dropping a packet at the *application* layer with decode-through advances
`dec_spk` exactly as Node A's encoder did, so the ADPCM stream never
desyncs. Drop without decode-through = static forever.

## Throughput: before vs after (verified)

| Counter | Arduino (before) | ESP-IDF (after port) |
| --- | --- | --- |
| `recv` | ~102/s | ~100/s |
| `played` | 41/s | **~100/s** |
| `dropthru` | ~61/s | **~0/s** |
| `isr_drop` | 0 | 0 |
| `qdepth` | 8–9 (pegged) | 0–2 (idle) |
| `Sent` (mic) | 41/s | ~40/s |

If `played` drops back to ~40/s, the I2S bottleneck has returned — re-check
that TX/RX are still separate channel handles and that nothing reintroduced a
shared mutex around I2S.

## Simultaneous operation (verified)

With both nodes running `flash_bridge.sh` and the host playing audio while
recording from the ESP UAC mic:

| Path | Rate | Healthy log |
| --- | --- | --- |
| Speaker (host → kit) | ~100 packets/s | `[Spk] recv:~100 played:~100 dropthru:0` |
| Mic (kit → host) | ~40 packets/s | `[Mic] Sent:~40 Peak:>0` (speak into mic) |

Core split is what makes this stable: **speaker_task on Core 1** is not
blocked by the 25 ms `i2s_channel_read` in **mic_task on Core 0**, and
neither shares the Arduino `i2sMutex`.

## Speaker gain chain (tunable in `main.c`)

| Constant | Default | Role |
| --- | --- | --- |
| `DAC_OUT_VOLUME` | 100 | `esp_codec_dev_set_out_vol` — UI 0–100 → ~−50…0 dB at DAC digital vol |
| `ES8388_LOUT_GAIN` | `0x21` | `DACCONTROL24/25` — +3 dB at output mixer (chip max per driver) |
| `SPK_PCM_GAIN_SHIFT` | 2 | Multiply decoded PCM by 4 before I2S (clip to int16) |

Host playback volume on the PC still sets how hot the wireless stream is on
Node A. If the kit distorts, lower `SPK_PCM_GAIN_SHIFT` to `1` or `0`.

## Post-migration debug journey

Chronological fixes after the first ESP-IDF build, in the order they appeared.

### 1. Compile: `es8388_codec_cfg_t` has no member `digital_mic` / `mic_amp`

**Symptom:** PlatformIO build failed against `esp_codec_dev` v1.3.x pulled by
`src/idf_component.yml`.

**Cause:** Older examples and the Arduino `AudioKitEs8388V1` API exposed input
routing as struct fields. The managed component removed those fields; routing
is register-based only.

**Fix:** Drop `digital_mic` and `mic_amp` from the `es8388_codec_cfg_t`
initializer. Select **LINE2** with a real I2C write (see §5 below — not
`esp_codec_dev_write_reg`).

### 2. Boot loop right after flash (no application logs)

**Symptom:** Serial showed bootloader + `cpu_start: Multicore app`, then
`Rebooting...` in a tight loop. No `bridge_b: Node_B_Bridge_idf` line.

**Cause:** ESP-IDF **5.4** + `esp_codec_dev` default
(`CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE=n`) uses the **new** I2C master driver.
`audio_codec_new_i2c_ctrl()` requires `audio_codec_i2c_cfg_t.bus_handle`. The
first port called only `i2c_driver_install()` (legacy API) and left
`bus_handle` NULL, so `audio_codec_new_i2c_ctrl()` returned NULL,
`codec_init()` failed, and `ESP_ERROR_CHECK(codec_init())` aborted before any
useful log line.

**Fix:**

```c
i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
// ...
audio_codec_i2c_cfg_t i2c_cfg = {
    .port       = I2C_NUM_0,
    .addr       = ES8388_I2C_ADDR_7BIT << 1,
    .bus_handle = s_i2c_bus,
};
```

**Alternative (not used):** set `CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE=y` in
`sdkconfig.defaults` to keep legacy `i2c_driver_install` — works, but the
`i2c_new_master_bus` path matches IDF 5.4 defaults.

**Expected boot log after fix:**

```
I (...) bridge_b: Node_B_Bridge_idf (full-duplex I2S, ES8388 via esp_codec_dev)
I (...) bridge_b: STA MAC ...
I (...) bridge_b: ES8388 ready: 48 kHz, LINE2 in, vol=100, LOUT=+3dB, PCM shift=2, mic_gain=6dB
I (...) bridge_b: Tasks running. spk Core 1 (prio 12), mic Core 0 (prio 8).
```

### 3. Full audio, but quiet

**Symptom:** Bridge worked end-to-end (continuous playback) but on-board
speaker level was noticeably lower than the Arduino sketch.

**Cause:** Multiple gain stages were under-used: `set_out_vol(70)` on a
0–100 curve (~−15 dB), LOUT registers never written (see §5), and no digital
PCM boost after ADPCM decode.

**Fix (layered):**

- `DAC_OUT_VOLUME` → **100** (`esp_codec_dev_set_out_vol`)
- `es8388_reg_write(DACCONTROL24/25, 0x21)` for +3 dB at the output mixer
- `SPK_PCM_GAIN_SHIFT` → **2** (×4 PCM, with int16 clipping)
- `[Spk] Peak:` in the stats line to see post-gain level (~5000–28000 when healthy)

### 4. Microphone stopped working

**Symptom:** Speaker still fine after volume tweaks; host UAC mic silent;
`[Mic] Peak:0` or very low while speaking.

**Cause:** `esp_codec_dev_write_reg()` does **not** work on the ES8388 —
`audio_codec_if_t` has no `set_reg` implementation, so the API returns
`ESP_CODEC_DEV_NOT_SUPPORTED` and only logs a warning. **LINE2 was never
selected**; the codec stayed on LINE1 while the A1S mic jack is wired to
LINE2. The same bug meant LOUT gain writes in §3 also had no effect until
fixed.

**Fix:**

- Keep `s_codec_ctrl` from `audio_codec_new_i2c_ctrl()`.
- Add `es8388_reg_write()` that calls `s_codec_ctrl->write_reg(...)`.
- After `esp_codec_dev_open`, write `ADCCONTROL2 = 0x50` (LINE2).
- Mic PGA: `esp_codec_dev_set_in_gain(dev, MIC_IN_GAIN_DB)` — default **6 dB**
  (Arduino bridge used 0 dB; 24 dB was louder but noisier through ADPCM).

```c
static esp_err_t es8388_reg_write(uint8_t reg, uint8_t val)
{
    int rc = s_codec_ctrl->write_reg(s_codec_ctrl, reg, 1, &val, 1);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}
```

If boot log shows `ADCCONTROL2 LINE2 select failed`, mic will stay silent on
this board — fix I2C before chasing ESP-NOW or Node A.

### Debug journey summary

| Step | Symptom | Root cause | Fix |
| --- | --- | --- | --- |
| Build | `digital_mic` / `mic_amp` errors | `esp_codec_dev` API change | `es8388_reg_write` for LINE2 |
| Runtime | Boot loop, no app logs | Missing `bus_handle` on IDF 5.4 I2C | `i2c_new_master_bus` + pass handle |
| Runtime | Quiet speaker | Low DAC vol + no LOUT/PCM boost | `DAC_OUT_VOLUME` 100, LOUT `0x21`, `SPK_PCM_GAIN_SHIFT` |
| Runtime | Mic dead | `esp_codec_dev_write_reg` no-op on ES8388 | `s_codec_ctrl->write_reg`, LINE2, `MIC_IN_GAIN_DB` |
| Runtime | Mic loud / noisy | High PGA + ADPCM | Lower `MIC_IN_GAIN_DB` in `main.c` (0–24 dB; default 6) |

## Files

```
Node_B_Bridge_idf/
├── BRIDGE_B_IDF_JOURNEY.md     ← this file
├── CMakeLists.txt              ← top-level project file
├── platformio.ini              ← board: esp32dev, framework: espidf
├── sdkconfig.defaults          ← FreeRTOS 1 kHz, WiFi RX buffers, BT off
└── src/
    ├── CMakeLists.txt          ← idf_component_register, REQUIRES list
    ├── idf_component.yml       ← esp_codec_dev + esphome/micro-opus
    └── main.c                  ← everything
```

## How to flash

```
./flash_bridge.sh
```

The script auto-detects the S3 port for Node A and the USB-UART port for
Node B and builds + uploads both projects via PlatformIO. Node B serial
output (the stats line, once/sec) is on `/dev/ttyUSB0` at 115200 8N1:

```
pio device monitor -d Node_B_Bridge_idf -b 115200 -p /dev/ttyUSB0
```

Example healthy line while playing audio and talking into the mic:

```
[Mic] Peak:8400 Sent:40 Fails:0 | [Spk] Peak:22000 recv:100 played:100 dropthru:0 isr_drop:0 qdepth:1
```

- `played: ~100/s`, `dropthru: 0` — speaker path healthy.
- `Sent: ~40/s`, `Peak` non-zero when speaking — mic path healthy.

If you see `played: ~40/s` again, re-run the bisection from
`Node_A_Bridge_idf/SPEAKER_BISECTION_JOURNEY.md` (Node A side) and confirm
Node B still uses separate `tx_chan` / `rx_chan` without an application mutex.

## Related docs

- `Node_A_Bridge_idf/BRIDGE_JOURNEY.md` — full bidirectional bridge (Node A UAC + ESP-NOW).
- `Node_A_Bridge_idf/SPEAKER_BISECTION_JOURNEY.md` — isolated speaker-path tests that led to the Node B port decision.

## Files that moved out of the way

The old Arduino sketch lives at `Node_B_Bridge/Node_B_Bridge.ino`. It is
still buildable via `arduino-cli` and intentionally kept as a fallback /
reference. Its `[Spk] frames/s: 41` cap is exactly what motivated this port.
