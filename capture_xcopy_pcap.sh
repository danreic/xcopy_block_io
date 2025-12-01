#!/bin/bash
# Script to capture and parse NVMe-TCP packets for XCOPY commands

DEVICE="/dev/nvme1n1"
SRC_LBA=0
DST_LBA=1937768448
BLOCKS=8192
NVME_PORT=4420  # Default NVMe-TCP port

echo "=== Capturing NVMe-TCP Packets for XCOPY ==="
echo ""

# Find the network interface (assuming it's the one with default route)
INTERFACE=$(ip route | grep default | awk '{print $5}' | head -1)
if [ -z "$INTERFACE" ]; then
    INTERFACE=$(route -n get default 2>/dev/null | grep interface | awk '{print $2}' | head -1)
fi

if [ -z "$INTERFACE" ]; then
    echo "ERROR: Could not determine network interface"
    exit 1
fi

echo "Using network interface: $INTERFACE"
echo "NVMe-TCP port: $NVME_PORT"
echo ""

# Function to get server IP from device
get_server_ip() {
    # Try to get IP from nvme list or device path
    # For NVMe-TCP, the device path might contain the IP, or we can check nvme list
    if command -v nvme &> /dev/null; then
        nvme list 2>/dev/null | grep "$DEVICE" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' | head -1
    fi
}

SERVER_IP=$(get_server_ip)
if [ -z "$SERVER_IP" ]; then
    echo "WARNING: Could not determine server IP. Will capture all traffic on port $NVME_PORT"
    FILTER="tcp port $NVME_PORT"
else
    echo "Server IP: $SERVER_IP"
    FILTER="tcp port $NVME_PORT and host $SERVER_IP"
fi

echo "Capture filter: $FILTER"
echo ""

# Test 1: Capture nvme-cli XCOPY command
echo "=== Test 1: Capturing nvme-cli XCOPY command ==="
echo "Command: sudo nvme copy $DEVICE --sdlba=$SRC_LBA --blocks=$((BLOCKS-1)) --slbs=$DST_LBA"
echo ""

# Write test data
echo "TEST_PCAP_$(date +%s)" | sudo dd of="$DEVICE" bs=512 seek=$SRC_LBA count=$BLOCKS 2>/dev/null

# Start packet capture in background
PCAP_NVME_CLI="/tmp/nvme_cli_xcopy.pcap"
sudo tcpdump -i "$INTERFACE" -w "$PCAP_NVME_CLI" "$FILTER" 2>/dev/null &
TCPDUMP_PID=$!
sleep 1

# Run nvme-cli command
sudo nvme copy "$DEVICE" --sdlba=$SRC_LBA --blocks=$((BLOCKS-1)) --slbs=$DST_LBA 2>&1

# Stop capture
sleep 1
sudo kill $TCPDUMP_PID 2>/dev/null
wait $TCPDUMP_PID 2>/dev/null

echo "Captured nvme-cli packets: $PCAP_NVME_CLI"
echo ""

# Test 2: Capture our xcopy_tool command
echo "=== Test 2: Capturing xcopy_tool XCOPY command ==="
echo "Command: sudo ./xcopy_tool --src-device $DEVICE --dst-device $DEVICE --src-lba $SRC_LBA --dst-lba $DST_LBA --range-size $BLOCKS --num-ranges 1 --count 1 --threads 1 --queue-depth 1"
echo ""

# Start packet capture in background
PCAP_XCOPY_TOOL="/tmp/xcopy_tool_xcopy.pcap"
sudo tcpdump -i "$INTERFACE" -w "$PCAP_XCOPY_TOOL" "$FILTER" 2>/dev/null &
TCPDUMP_PID=$!
sleep 1

# Run our tool
sudo ./xcopy_tool --src-device "$DEVICE" --dst-device "$DEVICE" --src-lba $SRC_LBA --dst-lba $DST_LBA --range-size $BLOCKS --num-ranges 1 --count 1 --threads 1 --queue-depth 1 -v 2>&1 | head -50

# Stop capture
sleep 1
sudo kill $TCPDUMP_PID 2>/dev/null
wait $TCPDUMP_PID 2>/dev/null

echo "Captured xcopy_tool packets: $PCAP_XCOPY_TOOL"
echo ""

# Analyze packets
echo "=== Packet Analysis ==="
echo ""

if command -v tshark &> /dev/null; then
    echo "--- nvme-cli packet summary ---"
    tshark -r "$PCAP_NVME_CLI" -Y "tcp.port == $NVME_PORT" -T fields -e frame.number -e ip.src -e ip.dst -e tcp.len 2>/dev/null | head -20
    echo ""
    
    echo "--- xcopy_tool packet summary ---"
    tshark -r "$PCAP_XCOPY_TOOL" -Y "tcp.port == $NVME_PORT" -T fields -e frame.number -e ip.src -e ip.dst -e tcp.len 2>/dev/null | head -20
    echo ""
    
    echo "--- Comparing packet sizes ---"
    echo "nvme-cli packets:"
    tshark -r "$PCAP_NVME_CLI" -Y "tcp.port == $NVME_PORT and tcp.len > 0" -T fields -e tcp.len 2>/dev/null | sort -n | uniq -c
    echo ""
    echo "xcopy_tool packets:"
    tshark -r "$PCAP_XCOPY_TOOL" -Y "tcp.port == $NVME_PORT and tcp.len > 0" -T fields -e tcp.len 2>/dev/null | sort -n | uniq -c
    echo ""
    
    echo "--- Extracting NVMe command data (first 100 bytes of data packets) ---"
    echo "nvme-cli (first data packet with payload > 64 bytes):"
    tshark -r "$PCAP_NVME_CLI" -Y "tcp.port == $NVME_PORT and tcp.len > 64" -T fields -e data 2>/dev/null | head -1 | xxd -r -p | xxd -l 100
    echo ""
    echo "xcopy_tool (first data packet with payload > 64 bytes):"
    tshark -r "$PCAP_XCOPY_TOOL" -Y "tcp.port == $NVME_PORT and tcp.len > 64" -T fields -e data 2>/dev/null | head -1 | xxd -r -p | xxd -l 100
else
    echo "tshark not found. Install wireshark/tshark for detailed analysis."
    echo "Basic packet counts:"
    echo "nvme-cli: $(tcpdump -r "$PCAP_NVME_CLI" 2>/dev/null | wc -l) packets"
    echo "xcopy_tool: $(tcpdump -r "$PCAP_XCOPY_TOOL" 2>/dev/null | wc -l) packets"
fi

echo ""
echo "=== PCAP Files ==="
echo "nvme-cli: $PCAP_NVME_CLI"
echo "xcopy_tool: $PCAP_XCOPY_TOOL"
echo ""
echo "You can analyze these files with:"
echo "  tshark -r $PCAP_NVME_CLI"
echo "  tshark -r $PCAP_XCOPY_TOOL"
echo "  wireshark $PCAP_NVME_CLI $PCAP_XCOPY_TOOL"

