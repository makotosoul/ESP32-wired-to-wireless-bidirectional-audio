# USB Microphone Debugging Journey

This document records how we diagnosed the failure of the Arduino `USBAudioCard` microphone path on ESP32-S3 (Linux / Audacity) and why the fix is a separate **PlatformIO + ESP-IDF** project using Espressif's `usb_device_uac` component.

---

## 1. Original goal

Build **Node A**: an ESP32-S3 that appears to the PC as a **USB microphone**, so Linux applications (Audacity, Google Meet) can record audio. Long term, that audio comes from a remote ESP32 over ESP-NOW (see `Node_A_Audio/`). Short term, we used a **synthetic square-wave tone** in a minimal debug sketch to prove the USB capture path works.

Related working path (different direction):

- `Node_A_Audio/Node_A_Audio.ino` — **USB speaker** (`UAC_SPK_STEREO`, `UAC_MIC_NONE`). Host → ESP works; audio is forwarded to another ESP32 over ESP-NOW. This does **not** validate device → host (microphone) capture.

---

## 2. Incremental tests and what each proved

### Iteration 1 — LED never blinked (`Node_A_USB_Debug_v2` initial)

| | |
|---|---|
| **Symptom** | After upload, no LED activity; board seemed dead. |
| **Hypothesis** | Global `USBAudioCard` constructor runs during C++ static init, before USB hardware is ready. |
| **Change** | `USBAudioCard* AudioCard`; construct with `new` in `setup()` after startup blinks (same pattern as `Node_A_USB_Debug` v1). |
| **Result** | Startup blinks returned; firmware reaches `setup()`. |

### Iteration 2 — Blinks but no audio in Audacity

| | |
|---|---|
| **Symptom** | Heartbeat LED works; Audacity shows device but no waveform / timeline stuck. |
| **Hypothesis** | `write()` blocks before host opens the isochronous IN stream. |
| **Tried** | `USB.connected()` — **does not exist** on `ESPUSB` in arduino-esp32 3.3.8. |
| **Tried** | Gate writes on `ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT` (`mic_stream_active`). |
| **Result** | LED could reflect stream open/close; still no audio. |

### Iteration 3 — Stream events fire; timeline still frozen

| | |
|---|---|
| **Symptom** | LED turns green when Audacity records; blue pulse when idle. |
| **Tried** | 1 ms pacing with `millis()` / `micros()`; `yield()`; 16 kHz; dynamic `sampleRate()`; FreeRTOS task + `vTaskDelay(1)`; pre-fill FIFO (write before stream open); remove gate again. |
| **Result** | Host **opens** the stream (events + LED) but **no PCM** reaches applications. |

### Iteration 4 — Visibility and flash procedure

| | |
|---|---|
| **Symptom** | Hard to see GPIO 4 LED; confusion about whether new firmware actually flashed. |
| **Change** | WS2812 RGB on GPIO 48; N16R8 FQBN (`PSRAM=opi`, `FlashSize=16M`); BOOT+RST before upload when device is USB-audio-only (no `/dev/ttyACM0` in runtime mode). |
| **Result** | Clear status: white boot flashes → blue idle pulse → green on record. Confirmed **our sketch runs** and **host negotiates the stream**. |

### Iteration 5 — Linux kernel diagnostics (decisive)

Commands run with board in USB-audio mode:

```bash
lsusb -v -d 303a:1001    # valid UAC1 mic descriptor
arecord -l                # card 1: ESP32S3DEV, device 0: USB Audio
arecord -D plughw:CARD=ESP32S3DEV,DEV=0 -f S16_LE -r 48000 -c 1 -d 5 /tmp/test.wav
```

| Observation | Meaning |
|-------------|---------|
| Descriptor: Audio class, 1 ch, 16-bit, 48000 Hz, isochronous IN EP | Firmware USB **descriptor** is correct |
| `arecord -l` lists the card | `snd-usb-audio` **bound** successfully |
| `arecord: pcm_read:2285: read error: Input/output error` | Kernel opens capture but **no valid isochronous data** arrives |

`dmesg` after plug-in showed normal enumeration (`ESP32S3_DEV`) and **no** `snd-usb-audio` error lines — failure is at **data transfer**, not enumeration.

---

## 3. Root cause

**Confirmed upstream bug:** [arduino-esp32 #12518 — USB Audio Card example Mic function is not working](https://github.com/espressif/arduino-esp32/issues/12518)

- Reported April 2026 on **v3.3.8** (same version we use).
- Still **open**; assigned to `@me-no-dev`.
- Reporter: speaker (host → device) works; mic (device → host) does not, including when **manually filling buffers** and calling `uac.write()` — same as our tests.
- Our work proved the problem is **not** Linux routing alone: direct `arecord` to ALSA hardware fails with I/O error.

The broken layer is the Arduino **`USBAudioCard::write()` → `tud_audio_write()`** path for microphone IN, not our tone generator or LED logic.

---

## 4. What we proved by elimination

| Layer | Status |
|-------|--------|
| MCU runs application code | Yes (RGB states, startup blinks) |
| USB descriptor | Yes (`lsusb -v`) |
| Kernel driver binding | Yes (`arecord -l`) |
| Host opens streaming interface | Yes (green LED / interface enable event) |
| Isochronous PCM delivery to host | **No** (`arecord` I/O error) |
| Arduino `USBAudioCard` mic `write()` | **Broken** (upstream issue) |

---

## 5. Chosen solution

Use Espressif's **`usb_device_uac`** component (ESP Component Registry `espressif/usb_device_uac`), which:

- Sits on TinyUSB but implements **SOF-driven callbacks**, ISO feedback, and proper mic FIFO pacing.
- Is maintained for ESP-IoT-Solution examples (not the broken Arduino wrapper).
- Exposes `uac_input_cb(buf, len, bytes_read, ctx)` — the host pulls audio; we fill the buffer and return `*bytes_read = len`.

**Toolchain:** PlatformIO + `framework = espidf` so the rest of the thesis can stay on Arduino while this one sketch uses ESP-IDF.

**This folder:** `Node_A_USB_Debug_v3_idf/`

| File | Role |
|------|------|
| `platformio.ini` | ESP32-S3 N16R8, upload port |
| `sdkconfig.defaults` | 48 kHz, mono mic, no speaker |
| `src/idf_component.yml` | Pull `usb_device_uac`, `led_strip` |
| `src/main.c` | Tone generator in `uac_input_cb`, RGB status |

---

## 6. How to build, flash, and test

### Prerequisites (Arch Linux)

```bash
sudo pacman -S platformio-core python-pip base-devel openssl libffi
```

If build fails with `No module named pip` / `MissingPackageManifestError` for `tool-esptoolpy`:

```bash
rm -rf ~/.platformio/packages/tool-esptoolpy
pio run
```

### Build

```bash
cd Node_A_USB_Debug_v3_idf
pio run
```

First build downloads ESP-IDF toolchain (~500 MB, one-time).

### Flash

When the board runs USB-audio firmware, `/dev/ttyACM0` may disappear. Use download mode:

1. Hold **BOOT**
2. Press and release **RST**
3. Release **BOOT**
4. `pio run -t upload`

### Verify on Linux

```bash
arecord -l
arecord -D plughw:CARD=ESP32S3,DEV=0 -f S16_LE -r 48000 -c 1 -d 5 /tmp/test.wav
# Expect ~480000 bytes, no I/O error
aplay /tmp/test.wav
# Expect ~480 Hz buzz (square wave)
```

In Audacity: select the ESP UAC input explicitly (not "default"), mono, 48000 Hz.

### LED meaning (GPIO 48 WS2812)

| Pattern | Meaning |
|---------|---------|
| 5× white flash | Boot / proof-of-life |
| Slow blue pulse | USB up, stream not active |
| Solid green | Capture active (mute callback / streaming) |

---

## 7. Next steps after this works

1. Replace synthetic tone in `uac_device_input_cb` with PCM from ESP-NOW (logic from `Node_A_Mic_Test` / `Node_A_Audio` receive path).
2. Keep Arduino sketches for Node B and ESP-NOW; only Node A USB dongle needs ESP-IDF if this path stays stable.

---

## 8. If v3 still fails

| Fallback | Notes |
|----------|--------|
| arduino-esp32 `master` branch | Possible fix after #12518 |
| USB CDC raw audio + host script | Not a standard mic for Meet/Audacity |
| External USB audio bridge (e.g. CM108) + ESP I2S | Hardware cost, reliable UAC |
| `esp-iot-solution` native `idf.py` example | Same component, full IDF workflow |

---

## 9. PlatformIO build issues encountered

### `No module named pip` → `MissingPackageManifestError: tool-esptoolpy`

PlatformIO's espressif32 platform runs `python -m pip install ...` during
post-install of `tool-esptoolpy`. On Arch the `python` package does not bundle
pip — you must install `python-pip` separately. When pip is missing the
post-install step fails silently, leaving the package half-installed (no
`package.json`), and the next build step blames `tool-esptoolpy`.

```bash
sudo pacman -S python-pip base-devel openssl libffi
rm -rf ~/.platformio/packages/tool-esptoolpy
pio run
```

### `Two environments with different actions were specified for the same target: ... usb_descriptors.c.o`

Upstream `usb_device_uac` 1.2.3 uses
`target_sources(${tusb_lib} PUBLIC ...)`. `PUBLIC` propagates the source via
`INTERFACE_SOURCES` to any target that links `tinyusb`. PlatformIO's SCons
ESP-IDF builder then generates two separate compile actions for the same
object file — one from the `tinyusb` target, one from the propagated
consumer with `USB_DEVICE_UAC_VER_*` defines added. Native `idf.py`
de-duplicates; PlatformIO does not.

Two attempts:

1. **First attempt — `pre:` extra_script** patched the `managed_components/`
   copy of `CMakeLists.txt` from `PUBLIC` to `PRIVATE` before each build.
   Failed: the IDF Component Manager re-validates managed components on every
   `pio run` invocation, detects the modification, and restores the original
   from cache — wiping the patch before CMake even reads it.

2. **Working solution — `override_path` vendoring.**
   Copy `managed_components/espressif__usb_device_uac/` to
   [`components/usb_device_uac/`](components/usb_device_uac/) inside the
   project, edit the line to `PRIVATE` once, and reference the local copy
   from [`src/idf_component.yml`](src/idf_component.yml):

   ```yaml
   espressif/usb_device_uac:
     version: "^1.2.3"
     override_path: "../components/usb_device_uac"
   ```

   `override_path` is the official IDF Component Manager mechanism for local
   forks; the manager treats the directory as authoritative and never touches
   it. The vendored copy lives under version control so the patch is part of
   the repo.

Additional observation: even with `PRIVATE`, `compile_commands.json` showed
the file in two compile groups (`__idf_espressif__tinyusb` and
`__idf_espressif__usb_device_uac`). PlatformIO's SCons builder
([`espidf.py`](../../../.platformio/platforms/espressif32@6.10.0/builder/frameworks/espidf.py)
`compile_source_files`, around line 791) generates one StaticObject per
compile_group, and the `.o` path is derived from the source path relative to
`managed_components/` — so two groups referencing the same file produce two
identical-target build actions, which SCons rejects. Native `idf.py`'s Ninja
build silently dedupes; PlatformIO does not. The override + PRIVATE patch
collapses this back to a single compile group.

---

## References

- [arduino-esp32 #12518](https://github.com/espressif/arduino-esp32/issues/12518)
- [usb_device_uac component](https://components.espressif.com/components/espressif/usb_device_uac)
- [ESP-IoT-Solution USB Device UAC docs](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_device/usb_device_uac.html)
- Previous Arduino attempts: `Node_A_USB_Debug_v2/Node_A_USB_Debug_v2.ino`
