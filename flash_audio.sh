#!/bin/bash

# Handle Ctrl+C (SIGINT) to stop the script immediately
trap "echo -e '\nExiting...'; exit" INT

# Configuration
BASE_DIR="/home/makotosoul/project/Thesis"

echo "Scanning for connected boards..."
JSON_DATA=$(arduino-cli board list --format json)
# Extract only ports that have a serial protocol
PORTS=($(echo "$JSON_DATA" | jq -r '.detected_ports[] | select(.port.protocol=="serial") | .port.address'))

if [ ${#PORTS[@]} -lt 2 ]; then
    echo "Error: Found less than 2 serial boards (${#PORTS[@]} found)."
    echo "Please ensure both the S3 DevKit and A1S Audio Kit are connected."
    exit 1
fi

get_board_name() {
    local port=$1
    echo "$JSON_DATA" | jq -r ".detected_ports[] | select(.port.address==\"$port\") | .matching_boards[0].name // \"Unknown Board\""
}

get_fqbn() {
    local port=$1
    local vid=$(echo "$JSON_DATA" | jq -r ".detected_ports[] | select(.port.address==\"$port\") | .port.properties.vid // \"\"")
    local pid=$(echo "$JSON_DATA" | jq -r ".detected_ports[] | select(.port.address==\"$port\") | .port.properties.pid // \"\"")
    
    # Precise match for ESP32-S3 (USB-JTAG/serial)
    if [ "$vid" == "0x303a" ] && [ "$pid" == "0x1001" ]; then
        # FORCE S3 SPECIFIC FQBN
        echo "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashMode=qio,FlashSize=4M,PSRAM=disabled"
        return
    fi

    local fqbn=$(echo "$JSON_DATA" | jq -r ".detected_ports[] | select(.port.address==\"$port\") | .matching_boards[0].fqbn // \"\"")
    if [ "$fqbn" == "null" ] || [ -z "$fqbn" ] || [ "$fqbn" == "esp32:esp32:esp32_family" ]; then
        fqbn="esp32:esp32:esp32"
    fi
    echo "$fqbn"
}

PORT_A=${PORTS[0]}
NAME_A=$(get_board_name $PORT_A)
FQBN_A=$(get_fqbn $PORT_A)

PORT_B=${PORTS[1]}
NAME_B=$(get_board_name $PORT_B)
FQBN_B=$(get_fqbn $PORT_B)

# Swap if necessary so PORT_A is the S3
if [[ $FQBN_A != *"esp32s3"* ]] && [[ $FQBN_B == *"esp32s3"* ]]; then
    TMP_PORT=$PORT_A; PORT_A=$PORT_B; PORT_B=$TMP_PORT
    TMP_NAME=$NAME_A; NAME_A=$NAME_B; NAME_B=$TMP_NAME
    TMP_FQBN=$FQBN_A; FQBN_A=$FQBN_B; FQBN_B=$TMP_FQBN
fi

echo "------------------------------------------------"
echo "Step 1: Flashing Node A (S3) to $PORT_A..."
echo "Compiling Node_A_Audio with FQBN: $FQBN_A"
BUILD_A="/tmp/arduino_build_audio_A"
mkdir -p "$BUILD_A"
if arduino-cli compile --clean --build-path "$BUILD_A" --fqbn $FQBN_A "$BASE_DIR/Node_A_Audio"; then
    echo "Uploading Node A..."
    arduino-cli upload -p $PORT_A --input-dir "$BUILD_A" --fqbn $FQBN_A "$BASE_DIR/Node_A_Audio"
else
    echo "Error: Compilation of Node A failed."
    exit 1
fi

echo "------------------------------------------------"
echo "Step 2: Flashing Node B (A1S) to $PORT_B..."
echo "Compiling Node_B_Audio with FQBN: $FQBN_B"
BUILD_B="/tmp/arduino_build_audio_B"
mkdir -p "$BUILD_B"
if arduino-cli compile --clean --build-path "$BUILD_B" --fqbn $FQBN_B "$BASE_DIR/Node_B_Audio"; then
    echo "Uploading Node B..."
    arduino-cli upload -p $PORT_B --input-dir "$BUILD_B" --fqbn $FQBN_B "$BASE_DIR/Node_B_Audio"
else
    echo "Error: Compilation of Node B failed."
    exit 1
fi

echo "------------------------------------------------"
echo "Success! Both nodes have been flashed."
