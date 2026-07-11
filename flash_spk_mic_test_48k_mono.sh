#!/bin/bash
#
# Bisection Test A for the bridge speaker bug.
#
# Flashes Node_A_Spk_Mic_Test_48k_mono_idf (48 kHz MONO spk + silent 48 kHz
# mono mic, N_AS_INT=2, same UAC layout as the bridge) on Node A + the
# unmodified Node_B_Audio on Node B.
#
# Only difference vs flash_spk_mic_test_48k.sh (Test B, white LED):
#   CONFIG_UAC_SPEAKER_CHANNEL_NUM: 2 -> 1

trap "echo -e '\nExiting...'; exit" INT

BASE_DIR="/home/makotosoul/project/Thesis"
NODE_A_PROJECT="$BASE_DIR/Node_A_Spk_Mic_Test_48k_mono_idf"
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

echo "Scanning for connected boards (Bisection Test A: 48 kHz MONO spk+silent-mic + Arduino B)..."
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
    SKIP_NODE_A=1
fi

echo "------------------------------------------------"
if [ -z "$SKIP_NODE_A" ]; then
    NAME_A=$(get_board_name "$S3_PORT")
    echo "Step 1: Flashing Node A (Test A: 48 kHz MONO spk+silent-mic) to $S3_PORT [$NAME_A]..."
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
echo "Step 2: Flashing Node B (A1S Audio, Arduino, unchanged) to $B_PORT [$NAME_B]..."
echo "Compiling Node_B_Audio with FQBN: $B_FQBN"
BUILD_B="/tmp/arduino_build_spk_mic_test_48k_mono_B"
mkdir -p "$BUILD_B"
if arduino-cli compile --clean --build-path "$BUILD_B" --fqbn "$B_FQBN" "$NODE_B_SKETCH"; then
    echo "Uploading Node B Audio..."
    arduino-cli upload -p "$B_PORT" --input-dir "$BUILD_B" --fqbn "$B_FQBN" "$NODE_B_SKETCH"
else
    echo "Error: Compilation of Node B Audio failed."
    exit 1
fi

echo "------------------------------------------------"
echo "Success! Test A pair flashed."
echo
echo "Run BOTH simultaneously so the host opens both AS interfaces:"
echo "  arecord -D plughw:CARD=Device,DEV=0 -f S16_LE -r 48000 -c 1 -d 30 /tmp/silent_48k_mono.wav &"
echo "  speaker-test -D plughw:CARD=Device,DEV=0 -c 1 -r 48000 -f 440 -t sine -l 1"
echo
echo "Important: use -c 1 (mono) for speaker-test -- this build is mono UAC."
echo "Audio on Node B may sound wrong (rate/channel mismatch). Ignore it;"
echo "only the Node A LED matters."
echo
echo "Node A LED diagnostic (speaker = ESP-NOW packet sent, not just USB callback):"
echo "  WHITE solid      = mic active + speaker packets reaching ESP-NOW."
echo "                     => Mono UAC + wireless path OK; bridge bug is app logic."
echo "  MAGENTA solid    = speaker packets only (no arecord)."
echo "  GREEN solid only = mic/arecord only; no ESP-NOW speaker packets."
echo "                     => USB speaker callback may fire but encode/send broken."
echo "  BLUE blink only  = host never opens streams."
echo "  RED blink fast   = ESP-NOW send failures."
echo
echo "Verify Node B: serial monitor should show ~140 kbps while audio plays."
