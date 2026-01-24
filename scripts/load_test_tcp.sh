#!/bin/bash

# TCP ingest load test
# Sends high-frequency events to test allocation overhead

PORT=9091
HOST=127.0.0.1
NUM_EVENTS=10000
PAYLOAD_SIZE=256

echo "═══════════════════════════════════════════════════════"
echo "TCP INGEST LOAD TEST"
echo "═══════════════════════════════════════════════════════"
echo "Target:       $HOST:$PORT"
echo "Events:       $NUM_EVENTS"
echo "Payload size: $PAYLOAD_SIZE bytes"
echo ""

# Function to create a test frame
create_frame() {
    local event_id=$1
    local topic="test.topic.$((event_id % 10))"  # 10 different topics
    local priority=$((event_id % 3))  # Random priority
    
    # Frame format: [4-byte length][JSON payload]
    local payload="{\"event_id\":$event_id,\"topic\":\"$topic\",\"priority\":$priority,\"timestamp\":$(date +%s%N)}"
    
    # Pad payload to fixed size
    local padded_payload=$(printf "%-${PAYLOAD_SIZE}s" "$payload")
    
    # Calculate frame length (payload only, not including the 4-byte length header)
    local frame_len=${#padded_payload}
    
    # Encode length as big-endian 4 bytes
    printf "\\x$(printf '%02x' $((frame_len >> 24)))\\x$(printf '%02x' $(((frame_len >> 16) & 0xFF)))\\x$(printf '%02x' $(((frame_len >> 8) & 0xFF)))\\x$(printf '%02x' $((frame_len & 0xFF)))"
    printf "$padded_payload"
}

# Send events
{
    for ((i=0; i<NUM_EVENTS; i++)); do
        create_frame $i
    done
} | nc -q 1 $HOST $PORT 2>/dev/null

if [ $? -eq 0 ]; then
    echo "✓ Successfully sent $NUM_EVENTS events"
else
    echo "✗ Failed to connect to $HOST:$PORT"
    exit 1
fi
