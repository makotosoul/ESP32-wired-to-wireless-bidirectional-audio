#!/bin/bash
#
# Bisection Test C for the bridge speaker bug.
#
# Flashes Node_A_Spk_Mic_Test_idf (44.1 kHz stereo spk + silent 44.1 kHz
# mono mic, both via patched usb_device_uac) on Node A, plus the
# unmodified Node_B_Audio.ino on Node B.
#
# Compared to flash_audio_idf.sh, this build also enables the mic
# interface (CONFIG_UAC_MIC_CHANNEL_NUM=1) so that the patched component
# resolves to CFG_TUD_AUDIO_FUNC_1_N_AS_INT=2 -- the same dual-AS-interface
# codepath that the bridge uses. The speaker side is unchanged.
#
# Goal: see whether having BOTH AS interfaces breaks the speaker callback
# in isolation. If yes -> we've reproduced the bridge bug minimally and
# the fix is in the patched usb_device_uac component.

trap "echo -e '\nExiting...'; exit" INT

BASE_DIR="/home/makotosoul/project/Thesis"
NODE_A_PROJECT="$BASE_DIR/Node_A_Spk_Mic_Test_idf"
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

echo "Scanning for connected boards (Bisection Test C: spk+silent-mic + Arduino B)..."
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
    echo "Step 1: Flashing Node A (Test C: spk+silent-mic, both 44.1 kHz) to $S3_PORT [$NAME_A]..."
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
BUILD_B="/tmp/arduino_build_spk_mic_test_B"
mkdir -p "$BUILD_B"
if arduino-cli compile --clean --build-path "$BUILD_B" --fqbn "$B_FQBN" "$NODE_B_SKETCH"; then
    echo "Uploading Node B Audio..."
    arduino-cli upload -p "$B_PORT" --input-dir "$BUILD_B" --fqbn "$B_FQBN" "$NODE_B_SKETCH"
else
    echo "Error: Compilation of Node B Audio failed."
    exit 1
fi

echo "------------------------------------------------"
echo "Success! Test C pair flashed."
echo
echo "Verify on Linux. Open BOTH a recorder and a player simultaneously"
echo "(so both UAC interfaces are activated by the host):"
echo "  aplay -l                                  # confirm 'ESP UAC Device' is present"
echo "  arecord -l                                # confirm mic side also enumerated"
echo "  # Recorder (silent input from mic): forces host to open the IN stream"
echo "  arecord -D plughw:CARD=Device,DEV=0 -f S16_LE -r 44100 -c 1 -d 30 /tmp/silent_mic.wav &"
echo "  # Player (real audio out): forces host to open the OUT stream"
echo "  speaker-test -D plughw:CARD=Device,DEV=0 -c 2 -r 44100 -f 440 -t sine -l 1"
echo
echo "Node A LED diagnostic (key thing to watch):"
echo "  Blue blink slow  = idle, no UAC callbacks firing"
echo "  Green solid      = ONLY mic callback firing (host pulled silence) -- WATCH FOR THIS"
echo "  Magenta solid    = ONLY speaker callback firing"
echo "  White solid      = both callbacks firing (the good state)"
echo "  Red blink fast   = ESP-NOW send failures"
echo
echo "Interpretation:"
echo "  WHITE + audio plays on Node B   -> N_AS_INT=2 path is FINE."
echo "                                     Bridge bug is in {48 kHz, mono, or both with mic}."
echo "                                     Next step: write Test B (48 kHz stereo)."
echo "  GREEN only (no audio on B)      -> REPRODUCED THE BRIDGE BUG MINIMALLY."
echo "                                     Patched usb_device_uac N_AS_INT=2 codepath is broken."
echo "                                     Next step: debug the vendored component."
echo "  BLUE only with arecord+aplay    -> host never opens streams. PipeWire / descriptor problem."
