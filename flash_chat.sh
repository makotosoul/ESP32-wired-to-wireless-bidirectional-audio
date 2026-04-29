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
    echo "Please ensure both the DevKit and Audio Kit are connected."
    exit 1
fi

get_board_name() {
    local port=$1
    echo "$JSON_DATA" | jq -r ".detected_ports[] | select(.port.address==\"$port\") | .matching_boards[0].name // \"Unknown Board\""
}

get_fqbn() {
    local port=$1
    # Extract VID and PID for precise matching
    local vid=$(echo "$JSON_DATA" | jq -r ".detected_ports[] | select(.port.address==\"$port\") | .port.properties.vid // \"\"")
    local pid=$(echo "$JSON_DATA" | jq -r ".detected_ports[] | select(.port.address==\"$port\") | .port.properties.pid // \"\"")
    
    # Precise match for ESP32-S3 (USB-JTAG/serial)
    if [ "$vid" == "0x303a" ] && [ "$pid" == "0x1001" ]; then
        # Enable CDC on boot so Serial.print goes to the USB port
        echo "esp32:esp32:esp32s3:CDCOnBoot=cdc"
        return
    fi

    # Try to find a matching board's FQBN in JSON
    local fqbn=$(echo "$JSON_DATA" | jq -r ".detected_ports[] | select(.port.address==\"$port\") | .matching_boards[0].fqbn // \"\"")
    
    # If FQBN is the generic "family" one or empty, default to standard esp32
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

echo "------------------------------------------------"
echo "Step 1: Flashing Node A to $PORT_A ($NAME_A)..."
echo "Compiling Node A (FQBN: $FQBN_A)..."
BUILD_A="/tmp/arduino_build_A"
mkdir -p "$BUILD_A"
if arduino-cli compile --clean --build-path "$BUILD_A" --fqbn $FQBN_A "$BASE_DIR/Node_A"; then
    echo "Uploading Node A..."
    arduino-cli upload -p $PORT_A --input-dir "$BUILD_A" --fqbn $FQBN_A "$BASE_DIR/Node_A"
else
    echo "Error: Compilation of Node A failed."
    exit 1
fi

echo "------------------------------------------------"
echo "Step 2: Flashing Node B to $PORT_B ($NAME_B)..."
echo "Compiling Node B (FQBN: $FQBN_B)..."
BUILD_B="/tmp/arduino_build_B"
mkdir -p "$BUILD_B"
if arduino-cli compile --clean --build-path "$BUILD_B" --fqbn $FQBN_B "$BASE_DIR/Node_B"; then
    echo "Uploading Node B..."
    arduino-cli upload -p $PORT_B --input-dir "$BUILD_B" --fqbn $FQBN_B "$BASE_DIR/Node_B"
else
    echo "Error: Compilation of Node B failed."
    exit 1
fi

echo "------------------------------------------------"
echo "Success! Both nodes have been flashed and are ready."
echo "Node A Port: $PORT_A"
echo "Node B Port: $PORT_B"
echo ""
echo "Workflow Complete:"
echo "1. Open a serial monitor for $PORT_A (115200 baud)"
echo "2. Open a serial monitor for $PORT_B (115200 baud)"
echo "3. Start chatting!"
