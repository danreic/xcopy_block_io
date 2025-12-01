#!/bin/bash
# Script to capture and parse NVMe-TCP packets for XCOPY commands

DEVICE="/dev/nvme1n1"
SRC_LBA=0
DST_LBA=1937768448
BLOCKS=8192
NVME_PORT=4420  # Default NVMe-TCP port

echo "=== Capturing NVMe-TCP Packets for XCOPY ==="
echo ""

# Function to list available interfaces
list_interfaces() {
    if command -v ip &> /dev/null; then
        ip link show | grep -E "^[0-9]+:" | awk -F': ' '{print $2}' | grep -v lo
    elif command -v ifconfig &> /dev/null; then
        ifconfig -l 2>/dev/null | tr ' ' '\n' | grep -v lo
    else
        ls /sys/class/net/ 2>/dev/null | grep -v lo
    fi
}

# Check if device is actually using TCP transport
check_transport() {
    if command -v nvme &> /dev/null; then
        nvme list 2>/dev/null | grep "$DEVICE" | grep -q "tcp"
        return $?
    fi
    return 1
}

echo "=== Network Interface Detection ==="
echo "Available interfaces:"
list_interfaces | while read iface; do
    echo "  - $iface"
done
echo ""

# Check if device uses TCP transport
if ! check_transport; then
    echo "WARNING: Device $DEVICE may not be using TCP transport!"
    echo "NVMe-TCP capture may not work. Checking nvme list output:"
    nvme list 2>/dev/null | grep "$DEVICE" || echo "  (device not found in nvme list)"
    echo ""
fi

# Try to find interface with NVMe-TCP traffic
# First, try to get interface from nvme connection info
INTERFACE=""
if command -v nvme &> /dev/null; then
    # Try to get connection info
    NVME_INFO=$(nvme list 2>/dev/null | grep "$DEVICE")
    echo "NVMe device info: $NVME_INFO"
    echo ""
fi

# If we can't determine, try common interfaces
if [ -z "$INTERFACE" ]; then
    # Try default route interface first
    INTERFACE=$(ip route | grep default | awk '{print $5}' | head -1 2>/dev/null)
    if [ -z "$INTERFACE" ]; then
        INTERFACE=$(route -n get default 2>/dev/null | grep interface | awk '{print $2}' | head -1)
    fi
    # If still empty, try first non-lo interface
    if [ -z "$INTERFACE" ]; then
        INTERFACE=$(list_interfaces | head -1)
    fi
fi

if [ -z "$INTERFACE" ]; then
    echo "ERROR: Could not determine network interface"
    echo "Please specify interface manually: INTERFACE=eth0 $0"
    exit 1
fi

# Allow override via environment variable
if [ -n "$CAPTURE_INTERFACE" ]; then
    INTERFACE="$CAPTURE_INTERFACE"
fi

echo "Using network interface: $INTERFACE"
echo "NVMe-TCP port: $NVME_PORT"
echo ""
echo "NOTE: If no packets are captured, try specifying the interface:"
echo "  CAPTURE_INTERFACE=eth0 sudo $0"
echo ""

# Function to get server IP from device
get_server_ip() {
    # Try to get IP from nvme list or device path
    # For NVMe-TCP, the device path might contain the IP, or we can check nvme list
    if command -v nvme &> /dev/null; then
        nvme list 2>/dev/null | grep "$DEVICE" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' | head -1
    fi
}

# Function to find interface with traffic to/from server IP
find_interface_for_ip() {
    local target_ip=$1
    if [ -z "$target_ip" ]; then
        return
    fi
    
    # Check which interface has routes to this IP
    if command -v ip &> /dev/null; then
        ip route get "$target_ip" 2>/dev/null | grep -oP 'dev \K\S+' | head -1
    elif command -v route &> /dev/null; then
        route get "$target_ip" 2>/dev/null | grep interface | awk '{print $2}' | head -1
    fi
}

SERVER_IP=$(get_server_ip)
if [ -n "$SERVER_IP" ]; then
    echo "Server IP: $SERVER_IP"
    # Try to find interface for this IP
    IP_INTERFACE=$(find_interface_for_ip "$SERVER_IP")
    if [ -n "$IP_INTERFACE" ] && [ "$IP_INTERFACE" != "$INTERFACE" ]; then
        echo "Found interface $IP_INTERFACE for IP $SERVER_IP"
        INTERFACE="$IP_INTERFACE"
    fi
    FILTER="tcp port $NVME_PORT and host $SERVER_IP"
else
    echo "WARNING: Could not determine server IP. Will capture all traffic on port $NVME_PORT"
    echo "This may capture traffic from other NVMe-TCP connections!"
    FILTER="tcp port $NVME_PORT"
fi

echo "Capture filter: $FILTER"
echo "Final interface: $INTERFACE"
echo ""

# Check if we can see any existing NVMe-TCP connections
echo "Checking for existing NVMe-TCP connections on port $NVME_PORT..."
if command -v ss &> /dev/null; then
    ss -tnp | grep ":$NVME_PORT" | head -5
elif command -v netstat &> /dev/null; then
    netstat -tnp 2>/dev/null | grep ":$NVME_PORT" | head -5
else
    echo "  (ss/netstat not available, skipping connection check)"
fi
echo ""

# Option to use 'any' interface to capture on all interfaces
if [ "$USE_ANY_INTERFACE" = "1" ]; then
    echo "Using 'any' interface to capture on all interfaces..."
    INTERFACE="any"
fi

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
sleep 2

# Run nvme-cli command
echo "Running nvme-cli command..."
sudo nvme copy "$DEVICE" --sdlba=$SRC_LBA --blocks=$((BLOCKS-1)) --slbs=$DST_LBA 2>&1
NVME_CLI_EXIT=$?

# Stop capture - find and kill tcpdump process
sleep 2
echo "Stopping packet capture..."
# Find tcpdump process writing to our file
TCPDUMP_PIDS=$(pgrep -f "tcpdump.*$PCAP_NVME_CLI" 2>/dev/null || true)
if [ -n "$TCPDUMP_PIDS" ]; then
    for pid in $TCPDUMP_PIDS; do
        sudo kill -TERM $pid 2>/dev/null || true
    done
    sleep 1
    # Force kill if still running
    for pid in $TCPDUMP_PIDS; do
        if kill -0 $pid 2>/dev/null; then
            sudo kill -KILL $pid 2>/dev/null || true
        fi
    done
fi

# Verify we captured packets
PACKET_COUNT=$(tcpdump -r "$PCAP_NVME_CLI" 2>/dev/null | wc -l)
echo "Captured nvme-cli packets: $PCAP_NVME_CLI ($PACKET_COUNT packets)"
if [ "$PACKET_COUNT" -eq 0 ]; then
    echo "WARNING: No packets captured! Try:"
    echo "  1. Use 'any' interface: USE_ANY_INTERFACE=1 sudo $0"
    echo "  2. Specify interface manually: CAPTURE_INTERFACE=eth0 sudo $0"
    echo "  3. Check if device is using TCP: nvme list | grep $DEVICE"
fi
echo ""

# Test 2: Capture our xcopy_tool command
echo "=== Test 2: Capturing xcopy_tool XCOPY command ==="
echo "Command: sudo ./xcopy_tool --src-device $DEVICE --dst-device $DEVICE --src-lba $SRC_LBA --dst-lba $DST_LBA --range-size $BLOCKS --num-ranges 1 --count 1 --threads 1 --queue-depth 1"
echo ""

# Start packet capture in background
PCAP_XCOPY_TOOL="/tmp/xcopy_tool_xcopy.pcap"
sudo tcpdump -i "$INTERFACE" -w "$PCAP_XCOPY_TOOL" "$FILTER" 2>/dev/null &
TCPDUMP_PID=$!
sleep 2

# Run our tool
echo "Running xcopy_tool..."
sudo ./xcopy_tool --src-device "$DEVICE" --dst-device "$DEVICE" --src-lba $SRC_LBA --dst-lba $DST_LBA --range-size $BLOCKS --num-ranges 1 --count 1 --threads 1 --queue-depth 1 -v 2>&1 | head -50
XCOPY_TOOL_EXIT=$?

# Stop capture - find and kill tcpdump process
sleep 2
echo "Stopping packet capture..."
# Find tcpdump process writing to our file
TCPDUMP_PIDS=$(pgrep -f "tcpdump.*$PCAP_XCOPY_TOOL" 2>/dev/null || true)
if [ -n "$TCPDUMP_PIDS" ]; then
    for pid in $TCPDUMP_PIDS; do
        sudo kill -TERM $pid 2>/dev/null || true
    done
    sleep 1
    # Force kill if still running
    for pid in $TCPDUMP_PIDS; do
        if kill -0 $pid 2>/dev/null; then
            sudo kill -KILL $pid 2>/dev/null || true
        fi
    done
fi

# Verify we captured packets
PACKET_COUNT=$(tcpdump -r "$PCAP_XCOPY_TOOL" 2>/dev/null | wc -l)
echo "Captured xcopy_tool packets: $PCAP_XCOPY_TOOL ($PACKET_COUNT packets)"
if [ "$PACKET_COUNT" -eq 0 ]; then
    echo "WARNING: No packets captured! Try:"
    echo "  1. Use 'any' interface: USE_ANY_INTERFACE=1 sudo $0"
    echo "  2. Specify interface manually: CAPTURE_INTERFACE=eth0 sudo $0"
    echo "  3. Check if device is using TCP: nvme list | grep $DEVICE"
fi
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

