#!/bin/bash
# Script to trace XCOPY commands and compare nvme-cli vs our tool

DEVICE="/dev/nvme1n1"
SRC_LBA=0
DST_LBA=1937768448
BLOCKS=8192

echo "=== Tracing XCOPY Commands ==="
echo ""

# Test 1: Trace nvme-cli command
echo "Test 1: Tracing nvme-cli XCOPY command..."
echo "Command: sudo nvme copy $DEVICE --sdlba=$SRC_LBA --blocks=$((BLOCKS-1)) --slbs=$DST_LBA"
echo ""

# Write test data first
echo "TEST_TRACE_$(date +%s)" | sudo dd of="$DEVICE" bs=512 seek=$SRC_LBA count=$BLOCKS 2>/dev/null

# Trace nvme-cli with strace (focus on ioctl calls)
echo "--- nvme-cli ioctl trace (first 50 lines) ---"
sudo strace -e trace=ioctl -s 200 nvme copy "$DEVICE" --sdlba=$SRC_LBA --blocks=$((BLOCKS-1)) --slbs=$DST_LBA 2>&1 | grep -A 5 -B 5 "ioctl\|NVME" | head -50
echo ""

# Test 2: Trace our tool
echo "Test 2: Tracing our xcopy_tool..."
echo "Command: sudo ./xcopy_tool --src-device $DEVICE --dst-device $DEVICE --src-lba $SRC_LBA --dst-lba $DST_LBA --range-size $BLOCKS --num-ranges 1 --count 1 --threads 1 --queue-depth 1"
echo ""

echo "--- xcopy_tool ioctl trace (first 50 lines) ---"
sudo strace -e trace=ioctl -s 200 ./xcopy_tool --src-device "$DEVICE" --dst-device "$DEVICE" --src-lba $SRC_LBA --dst-lba $DST_LBA --range-size $BLOCKS --num-ranges 1 --count 1 --threads 1 --queue-depth 1 2>&1 | grep -A 5 -B 5 "ioctl\|NVME" | head -50
echo ""

echo "=== Comparison ==="
echo "Compare the ioctl calls above to see if there are differences in:"
echo "1. The ioctl number (NVME_IOCTL_IO_CMD vs NVME_IOCTL_ADMIN_CMD)"
echo "2. The command structure (opcode, CDW values)"
echo "3. The data buffer address and length"
echo ""

