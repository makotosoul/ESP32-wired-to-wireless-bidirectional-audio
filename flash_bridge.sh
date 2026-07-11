#!/bin/bash

# Handle Ctrl+C (SIGINT) to stop the script immediately
trap "echo -e '\nExiting...'; exit" INT

BASE_DIR="/home/makotosoul/project/Thesis"
NODE_A_PROJECT="$BASE_DIR/Node_A_Bridge_idf"
NODE_B_PROJECT="$BASE_DIR/Node_B_Bridge_idf"

require() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "Error: '$1' not found in PATH. Install it and re-run."
        exit 1
    }
}
require arduino-cli
require jq
require pio

echo "Scanning for connected boards (Bidirectional Audio Bridge)..."
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
for p in "${PORTS[@]}"; do
    if is_s3_port "$p"; then
        S3_PORT="$p"
    else
        if [ -z "$B_PORT" ]; then
            B_PORT="$p"
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
    echo "Step 1: Flashing Node A (S3, ESP-IDF bridge) to $S3_PORT [$NAME_A]..."
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
echo "Step 2: Flashing Node B (A1S, ESP-IDF bridge) to $B_PORT [$NAME_B]..."
echo "Project: $NODE_B_PROJECT"
if pio run -d "$NODE_B_PROJECT" -t upload --upload-port "$B_PORT"; then
    echo "Node B flashed."
else
    echo "Error: PlatformIO upload of Node B failed."
    exit 1
fi

echo "------------------------------------------------"
echo "Success! Bidirectional bridge nodes flashed."
echo
echo "Verify on Linux (48 kHz mono UAC headset):"
echo "  arecord -l"
echo "  # Mic (A1S -> host):"
echo "  arecord -D plughw:CARD=Device,DEV=0 -f S16_LE -r 48000 -c 1 -d 5 /tmp/mic.wav"
echo "  aplay /tmp/mic.wav"
echo "  # Speaker (host -> A1S): play any 48 kHz mono WAV to the same card"
echo "  aplay -D plughw:CARD=Device,DEV=0 -f S16_LE -r 48000 -c 1 /path/to/test.wav"
echo
echo "Node A LED: white boot -> blue idle -> green mic / magenta spk / white both"
echo
echo "Node B serial log (throughput stats, once/sec):"
echo "  pio device monitor -d $NODE_B_PROJECT -b 115200 -p $B_PORT"
