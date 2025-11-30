#!/bin/bash
# Test XCOPY using nvme-cli to verify server support

DEVICE="/dev/nvme1n1"
SRC_LBA=0
DST_LBA=1937768448
BLOCKS=8192

echo "Testing XCOPY with nvme-cli..."
echo "Device: $DEVICE"
echo "Source LBA: $SRC_LBA"
echo "Destination LBA: $DST_LBA"
echo "Blocks: $BLOCKS"
echo ""

# Check if nvme command exists
if ! command -v nvme &> /dev/null; then
    echo "ERROR: nvme-cli not found. Please install it first."
    exit 1
fi

# Write test data
echo "Step 1: Writing test data to source..."
echo "TEST_DATA_$(date +%s)" | sudo dd of=/dev/nvme1n1 bs=512 seek=$SRC_LBA count=$BLOCKS 2>/dev/null

# Try XCOPY with nvme-cli
echo "Step 2: Attempting XCOPY with nvme-cli..."
echo "Command: sudo nvme copy $DEVICE --sdlba=$SRC_LBA --blocks=$((BLOCKS-1)) --slbs=$DST_LBA"
sudo nvme copy "$DEVICE" --sdlba=$SRC_LBA --blocks=$((BLOCKS-1)) --slbs=$DST_LBA 2>&1

if [ $? -eq 0 ]; then
    echo "✓ nvme-cli XCOPY command succeeded"
    
    # Read and compare
    echo "Step 3: Reading destination and comparing..."
    sudo dd if="$DEVICE" of=/tmp/nvme_src.bin bs=512 skip=$SRC_LBA count=$BLOCKS 2>/dev/null
    sudo dd if="$DEVICE" of=/tmp/nvme_dst.bin bs=512 skip=$DST_LBA count=$BLOCKS 2>/dev/null
    
    if cmp -s /tmp/nvme_src.bin /tmp/nvme_dst.bin; then
        echo "✓ SUCCESS: Data matches! Server supports XCOPY."
    else
        echo "✗ FAIL: Data doesn't match. XCOPY may not be working correctly."
    fi
    
    rm -f /tmp/nvme_src.bin /tmp/nvme_dst.bin
else
    echo "✗ nvme-cli XCOPY command failed"
    echo "This suggests the server may not support XCOPY, or the command format is incorrect."
fi

