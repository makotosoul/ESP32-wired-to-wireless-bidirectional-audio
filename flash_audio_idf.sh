#!/bin/bash
#
# Isolation test: flash the new IDF/PlatformIO speaker-only Node A
# (Node_A_Audio_idf) + the unmodified Arduino Node_B_Audio.
#
# Purpose: prove whether the patched usb_device_uac component's speaker
# OUT path works in IDF without the mic interface present. Pairs with
# Node_B_Audio.ino which we know decodes 240 B / 44.1 kHz stereo ADPCM
# packets correctly (it was the receiver for the old Node_A_Audio.ino).
#
# Layout mirrors flash_bridge.sh so the auto-detect logic stays familiar.

trap "echo -e '\nExiting...'; exit" INT

BASE_DIR="/home/makotosoul/project/Thesis"
NODE_A_PROJECT="$BASE_DIR/Node_A_Audio_idf"
NODE_B_SKETCH="$BASE_DIR/Node_B_Audio"

require() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "Error: '$1' not found in PATH. Install it and re-run."
        exit 1
    }
}
require arduino-cli
require jq
require pio

echo "Scanning for connected boards (Audio Isolation Test: IDF spk + Arduino B)..."
JSON_DATA=$(arduino-cli board list --format json)
PORTS=($(echo "$JSON_DATA" | jq -r '.detected_ports[] | select(.port.protocol=="serial") | .port.address'))

get_board_name() {
    local port=$1
    echo "$JSON_DATA" | jq -r ".detected_ports[] | select(.port.address==\"$port\") | .matching_boards[0].name // \"Unknown Board\""
}

get_vid() {
    local port=$1
    echo "$JSON_DATA" | jq -r ".detected_ports[] | select(.port.address==\"$port\") | .port.properties.vid // \"\""
}

get_pid() {
    local port=$1
    echo "$JSON_DATA" | jq -r ".detected_ports[] | select(.port.address==\"$port\") | .port.properties.pid // \"\""
}

is_s3_port() {
    local port=$1
    [ "$(get_vid $port)" == "0x303a" ] && [ "$(get_pid $port)" == "0x1001" ]
}

get_fqbn() {
    local port=$1
    if is_s3_port "$port"; then
        echo "esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=default,FlashMode=qio,FlashSize=16M,PSRAM=opi"
        return
    fi
    local fqbn=$(echo "$JSON_DATA" | jq -r ".detected_ports[] | select(.port.address==\"$port\") | .matching_boards[0].fqbn // \"\"")
    if [ "$fqbn" == "null" ] || [ -z "$fqbn" ] || [ "$fqbn" == "esp32:esp32:esp32_family" ]; then
        fqbn="esp32:esp32:esp32"
    fi
    echo "$fqbn"
}

S3_PORT=""
B_PORT=""
B_FQBN=""
for p in "${PORTS[@]}"; do
    if is_s3_port "$p"; then
        S3_PORT="$p"
    else
        if [ -z "$B_PORT" ]; then
            B_PORT="$p"
            B_FQBN=$(get_fqbn "$p")
        fi
    fi
done

if [ -z "$S3_PORT" ] && [ -z "$B_PORT" ]; then
    echo "Error: no serial boards detected."
    echo "Plug in the S3 dongle (Node A) and the A1S Audio Kit (Node B) and re-run."
    exit 1
fi

if [ -z "$S3_PORT" ]; then
    cat <<EOF
Warning: no S3 serial port (VID 0x303a:0x1001) detected.
Expected when Node A is already running USB-audio firmware (/dev/ttyACM0 hidden).

To flash Node A:
  1. Hold BOOT on the S3
  2. Tap RST
  3. Release BOOT
  4. Re-run this script (or: pio run -d $NODE_A_PROJECT -t upload)
EOF
    if [ -z "$B_PORT" ]; then
        exit 1
    fi
    echo "Continuing with Node B flash only..."
    SKIP_NODE_A=1
fi

echo "------------------------------------------------"
if [ -z "$SKIP_NODE_A" ]; then
    NAME_A=$(get_board_name "$S3_PORT")
    echo "Step 1: Flashing Node A (S3, ESP-IDF speaker-only @ 44.1 kHz stereo) to $S3_PORT [$NAME_A]..."
    echo "Project: $NODE_A_PROJECT"
    if pio run -d "$NODE_A_PROJECT" -t upload --upload-port "$S3_PORT"; then
        echo "Node A flashed."
    else
        echo "Error: PlatformIO upload of Node A failed."
        echo "Hint: hold BOOT, tap RST, release BOOT, then re-run."
        exit 1
    fi
else
    echo "Step 1: SKIPPED (no S3 serial port visible)."
fi

echo "------------------------------------------------"
if [ -z "$B_PORT" ]; then
    echo "Error: no Node B serial port detected."
    exit 1
fi

NAME_B=$(get_board_name "$B_PORT")
echo "Step 2: Flashing Node B (A1S Audio, Arduino) to $B_PORT [$NAME_B]..."
echo "Compiling Node_B_Audio with FQBN: $B_FQBN"
BUILD_B="/tmp/arduino_build_audio_iso_B"
mkdir -p "$BUILD_B"
if arduino-cli compile --clean --build-path "$BUILD_B" --fqbn "$B_FQBN" "$NODE_B_SKETCH"; then
    echo "Uploading Node B Audio..."
    arduino-cli upload -p "$B_PORT" --input-dir "$BUILD_B" --fqbn "$B_FQBN" "$NODE_B_SKETCH"
else
    echo "Error: Compilation of Node B Audio failed."
    exit 1
fi

echo "------------------------------------------------"
echo "Success! Audio isolation pair flashed (IDF Node A spk + Arduino Node B audio)."
echo
echo "Verify on Linux (44.1 kHz stereo UAC sink):"
echo "  aplay -l                                  # confirm 'ESP UAC Device' is present"
echo "  # Bypass PipeWire (cleanest test):"
echo "  speaker-test -D plughw:CARD=Device,DEV=0 -c 2 -r 44100 -f 440 -t sine -l 1"
echo "  # Or play a 44.1 kHz stereo WAV:"
echo "  aplay -D plughw:CARD=Device,DEV=0 -f S16_LE -r 44100 -c 2 /path/to/test.wav"
echo
echo "Node A LED diagnostic (use this since /dev/ttyACM0 is hidden by UAC):"
echo "  Blue blink slow   = idle, no UAC bytes from host"
echo "  Green solid       = uac_output_cb firing (host IS sending audio)"
echo "  Magenta solid     = ESP-NOW sends acknowledged"
echo "  White solid       = end-to-end OK (the goal)"
echo "  Red blink fast    = ESP-NOW send failures"
echo
echo "Diagnostic interpretation:"
echo "  - White       => speaker path works in IDF; bridge bug is in the combined mic+spk config."
echo "  - Green only  => host delivers bytes but ESP-NOW fails (peer MAC / channel / Node B asleep)."
echo "  - Blue only   => host never starts streaming (USB descriptor or PipeWire issue)."
