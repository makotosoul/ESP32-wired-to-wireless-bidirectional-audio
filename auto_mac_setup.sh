#!/bin/bash

# auto_mac_setup.sh — Discover ESP32 MAC addresses and inject them into
# peers.h and the IDF bridge source files (Node_A/Node_B main.c).
#
# Portable: no hardcoded home paths. Resolves everything relative to this
# script's location and finds esptool from PATH.

# Handle Ctrl+C (SIGINT) to stop the script immediately
trap "echo -e '\nExiting...'; exit" INT

# ===================== Resolve project root =====================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$SCRIPT_DIR"

# ===================== Find esptool =====================
find_esptool() {
    # Check PATH for esptool.py or esptool
    # (pip, PlatformIO, and Arduino ESP32 board support all put it in PATH)
    for cmd in esptool.py esptool; do
        if command -v "$cmd" >/dev/null 2>&1; then
            echo "$cmd"
            return 0
        fi
    done

    # Not found — prompt user with install instructions
    echo "" >&2
    echo "Error: esptool not found in PATH." >&2
    echo "" >&2
    echo "Install it using one of these methods:" >&2
    echo "  pip install esptool          # Python pip (recommended)" >&2
    echo "  pip install platformio       # PlatformIO (includes esptool)" >&2
    echo "  # Or install Arduino ESP32 board support (includes esptool)" >&2
    return 1
}

ESPTOOL=$(find_esptool)
if [ $? -ne 0 ]; then
    exit 1
fi
echo "Using esptool: $(command -v "$ESPTOOL")"

# ===================== Detect serial ports =====================
USE_ARDUINO_CLI=0
JSON_DATA=""

detect_serial_ports() {
    # Try arduino-cli first (best: gives VID/PID/board name for auto-identification)
    if command -v arduino-cli >/dev/null 2>&1 && command -v jq >/dev/null 2>&1; then
        JSON_DATA=$(arduino-cli board list --format json)
        PORTS=($(echo "$JSON_DATA" | jq -r \
            '.detected_ports[] | select(.port.protocol=="serial") | .port.address'))
        if [ ${#PORTS[@]} -ge 1 ]; then
            USE_ARDUINO_CLI=1
            return 0
        fi
    fi

    # Fallback: scan /dev/ttyUSB* and /dev/ttyACM*
    PORTS=()
    for dev in /dev/ttyUSB* /dev/ttyACM*; do
        [ -e "$dev" ] && PORTS+=("$dev")
    done
    USE_ARDUINO_CLI=0

    if [ ${#PORTS[@]} -lt 1 ]; then
        echo "Error: no serial ports found."
        echo "Plug in your ESP32 boards and try again."
        exit 1
    fi
}

echo "Scanning for connected boards..."
detect_serial_ports

if [ ${#PORTS[@]} -lt 2 ]; then
    echo "Error: Found less than 2 serial ports (${#PORTS[@]} found)."
    echo "Please ensure both the DevKit and Audio Kit are connected."
    exit 1
fi

# ===================== Identify Node A vs Node B =====================
get_board_name() {
    local port=$1
    if [ "$USE_ARDUINO_CLI" -eq 1 ]; then
        echo "$JSON_DATA" | jq -r ".detected_ports[] | select(.port.address==\"$port\") | .matching_boards[0].name // \"Unknown Board\""
    else
        echo "Unknown Board"
    fi
}

get_vid() {
    local port=$1
    if [ "$USE_ARDUINO_CLI" -eq 1 ]; then
        echo "$JSON_DATA" | jq -r ".detected_ports[] | select(.port.address==\"$port\") | .port.properties.vid // \"\""
    else
        echo ""
    fi
}

get_pid() {
    local port=$1
    if [ "$USE_ARDUINO_CLI" -eq 1 ]; then
        echo "$JSON_DATA" | jq -r ".detected_ports[] | select(.port.address==\"$port\") | .port.properties.pid // \"\""
    else
        echo ""
    fi
}

is_s3_port() {
    local port=$1
    [ "$(get_vid "$port")" == "0x303a" ] && [ "$(get_pid "$port")" == "0x1001" ]
}

assign_ports() {
    if [ "$USE_ARDUINO_CLI" -eq 1 ]; then
        # Auto-identify S3 (Node A) by VID/PID; everything else is Node B
        for p in "${PORTS[@]}"; do
            if is_s3_port "$p"; then
                PORT_A="$p"
            elif [ -z "$PORT_B" ]; then
                PORT_B="$p"
            fi
        done

        # If auto-detection picked both, we're done
        if [ -n "$PORT_A" ] && [ -n "$PORT_B" ]; then
            return 0
        fi

        # If only one was identified, assign the other
        if [ -n "$PORT_A" ] && [ -z "$PORT_B" ]; then
            for p in "${PORTS[@]}"; do
                if [ "$p" != "$PORT_A" ]; then
                    PORT_B="$p"
                    break
                fi
            done
            return 0
        fi

        # Fall through to manual if VID/PID didn't match anything
    fi

    # Manual fallback: prompt user to pick
    echo ""
    echo "Could not auto-detect board types. Please assign ports manually."
    echo ""
    echo "Detected serial ports:"
    for i in "${!PORTS[@]}"; do
        echo "  [$i] ${PORTS[$i]}"
    done
    echo ""
    read -p "Which port is Node A (S3 DevKit)? [0-$((${#PORTS[@]}-1))]: " idx_a
    read -p "Which port is Node B (A1S Audio Kit)? [0-$((${#PORTS[@]}-1))]: " idx_b

    if [ "$idx_a" == "$idx_b" ]; then
        echo "Error: Node A and Node B cannot be the same port."
        exit 1
    fi

    PORT_A="${PORTS[$idx_a]}"
    PORT_B="${PORTS[$idx_b]}"
}

PORT_A=""
PORT_B=""
assign_ports

if [ -z "$PORT_A" ] || [ -z "$PORT_B" ]; then
    echo "Error: could not assign both Node A and Node B ports."
    echo "Ensure both boards are plugged in and try again."
    exit 1
fi

NAME_A=$(get_board_name "$PORT_A")
NAME_B=$(get_board_name "$PORT_B")

echo "------------------------------------------------"
echo "Node A: $NAME_A on $PORT_A"
echo "Node B: $NAME_B on $PORT_B"
echo "------------------------------------------------"

# ===================== Read MAC addresses =====================
get_mac() {
    local port=$1
    local name=$2

    echo "------------------------------------------------" >&2
    echo "Reading hardware MAC from $name ($port)..." >&2

    local upload_log=$(mktemp)
    # read_mac is a built-in esptool command that reads from eFuse — no flashing needed
    $ESPTOOL --port "$port" read_mac > "$upload_log" 2>&1

    # The output contains a line like: "MAC: 1c:db:d4:7d:7c:24"
    local mac_line=$(grep "MAC:" "$upload_log" | head -n 1)
    rm -f "$upload_log"

    local mac=""
    if [ -n "$mac_line" ]; then
        mac=$(echo "$mac_line" | awk '{print $NF}' | tr -d '\r' | xargs)
    fi

    if [ -z "$mac" ]; then
        echo "Error: Failed to read MAC from $port" >&2
        return 1
    fi

    echo "  -> Found MAC: $mac" >&2
    echo "$mac"
}

MAC_A=$(get_mac "$PORT_A" "$NAME_A")
if [ $? -ne 0 ]; then
    echo "Failed to get MAC for Node A"
    exit 1
fi

MAC_B=$(get_mac "$PORT_B" "$NAME_B")
if [ $? -ne 0 ]; then
    echo "Failed to get MAC for Node B"
    exit 1
fi

# ===================== Format MACs =====================
format_mac() {
    echo "$1" | awk -F: '{printf "0x%s, 0x%s, 0x%s, 0x%s, 0x%s, 0x%s", $1, $2, $3, $4, $5, $6}'
}

FORMATTED_MAC_A=$(format_mac "$MAC_A")
FORMATTED_MAC_B=$(format_mac "$MAC_B")

# ===================== Write peers.h =====================
PEERS_FILE="$BASE_DIR/peers.h"
echo "Injecting MAC addresses into $PEERS_FILE..."

cat <<INNEREOF > "$PEERS_FILE"
#ifndef PEERS_H
#define PEERS_H

/*
 * Auto-generated by auto_mac_setup.sh
 * Generated on: $(date)
 *
 * Raw MAC byte macros — use these to initialise arrays with any name/qualifiers.
 */

#define PEER_MAC_A  $FORMATTED_MAC_A
#define PEER_MAC_B  $FORMATTED_MAC_B

/* Arduino-compatible arrays (Node_A, Node_B, Node_A_Audio, Node_B_Audio) */
static const uint8_t nodeAAddress[] = {PEER_MAC_A};
static const uint8_t nodeBAddress[] = {PEER_MAC_B};

#endif
INNEREOF

echo "  -> peers.h updated."

# ===================== Done =====================
echo ""
echo "================================================"
echo "Success! peers.h has been updated."
echo ""
echo "  Node A MAC: $MAC_A"
echo "  Node B MAC: $MAC_B"
echo ""
echo "All projects (Arduino and IDF) include peers.h —"
echo "recompile to pick up the new MACs:"
echo "  ./flash_bridge.sh       # Flash both IDF bridge nodes"
echo "================================================"

