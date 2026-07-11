# Node A USB Debug v3 (ESP-IDF)

USB UAC microphone test using Espressif `usb_device_uac` (bypasses broken Arduino `USBAudioCard` mic path).

See [DEBUGGING_JOURNEY.md](DEBUGGING_JOURNEY.md) for full diagnosis history.

## Setup (one-time, Arch Linux)

PlatformIO’s ESP32 tools run a post-install script that needs **pip**. Without it you get `No module named pip` and then `MissingPackageManifestError` for `tool-esptoolpy`.

```bash
sudo pacman -S platformio-core python-pip base-devel openssl libffi
```

Do **not** use `apt install python3-dev` — that is Debian/Ubuntu. On Arch, headers ship with the `python` package; `base-devel` covers build tools.

If you already hit the esptool error, remove the half-installed package and rebuild:

```bash
rm -rf ~/.platformio/packages/tool-esptoolpy
cd ~/project/Thesis/Node_A_USB_Debug_v3_idf
pio run
```

## Known issue: usb_device_uac 1.2.3 duplicate-target on PlatformIO

PlatformIO's ESP-IDF builder rejects a `target_sources(${tusb_lib} PUBLIC ...)`
call in `managed_components/espressif__usb_device_uac/CMakeLists.txt`. Even
with `PRIVATE`, PlatformIO's SCons generates two compile actions for the same
`usb_descriptors.c.o` (native `idf.py` de-duplicates; PlatformIO does not).

**Fix in this project:** a vendored copy of the component lives in
[`components/usb_device_uac/`](components/usb_device_uac/) with the offending
line patched. [`src/idf_component.yml`](src/idf_component.yml) pulls it via
`override_path`, so the IDF Component Manager never re-fetches or reverts the
patch. Other dependencies (`espressif/tinyusb`, `espressif/led_strip`,
`espressif/cmake_utilities`) continue to be fetched from the registry into
`managed_components/`.

If you ever need to re-vendor from a newer release:

```bash
# Delete the old vendor + cached build, then fetch and patch fresh
rm -rf components/usb_device_uac .pio/build managed_components/espressif__usb_device_uac
# Temporarily remove the override_path line in src/idf_component.yml and run:
pio run    # fetches managed_components/espressif__usb_device_uac
cp -r managed_components/espressif__usb_device_uac components/usb_device_uac
sed -i 's|PUBLIC "${COMPONENT_DIR}/tusb/usb_descriptors.c"|PRIVATE "${COMPONENT_DIR}/tusb/usb_descriptors.c"|' components/usb_device_uac/CMakeLists.txt
# Re-add the override_path entry, then build normally.
```

## Build

```bash
cd Node_A_USB_Debug_v3_idf
pio run
```

First build downloads the ESP-IDF toolchain (~500 MB).

## Flash

When running USB-audio firmware, use download mode: **BOOT** hold → **RST** tap → release **BOOT**, then:

```bash
pio run -t upload
```

## Test (Linux)

```bash
arecord -l
arecord -D plughw:CARD=ESP32S3,DEV=0 -f S16_LE -r 48000 -c 1 -d 5 /tmp/test.wav
aplay /tmp/test.wav
```

Replace `ESP32S3` with the card name from `arecord -l` if different.
