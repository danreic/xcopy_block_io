#!/bin/bash
# Detailed comparison of ioctl calls between nvme-cli and our tool

DEVICE="/dev/nvme1n1"
SRC_LBA=0
DST_LBA=1937768448
BLOCKS=8192

echo "=== Detailed ioctl Comparison ==="
echo ""

# Write test data
echo "TEST_COMPARE_$(date +%s)" | sudo dd of="$DEVICE" bs=512 seek=$SRC_LBA count=$BLOCKS 2>/dev/null

echo "=== nvme-cli ioctl call ==="
echo "Command: sudo nvme copy $DEVICE --sdlba=$SRC_LBA --blocks=$((BLOCKS-1)) --slbs=$DST_LBA"
echo ""

# Capture nvme-cli ioctl with full structure dump
sudo strace -e trace=ioctl -s 1000 -xx nvme copy "$DEVICE" --sdlba=$SRC_LBA --blocks=$((BLOCKS-1)) --slbs=$DST_LBA 2>&1 | grep -A 20 "NVME_IOCTL_IO_CMD" | head -30
echo ""

echo "=== xcopy_tool ioctl call ==="
echo "Command: sudo ./xcopy_tool --src-device $DEVICE --dst-device $DEVICE --src-lba $SRC_LBA --dst-lba $DST_LBA --range-size $BLOCKS --num-ranges 1 --count 1 --threads 1 --queue-depth 1"
echo ""

# Capture our tool's ioctl with full structure dump
sudo strace -e trace=ioctl -s 1000 -xx -f -o /tmp/xcopy_ioctl.log ./xcopy_tool --src-device "$DEVICE" --dst-device "$DEVICE" --src-lba $SRC_LBA --dst-lba $DST_LBA --range-size $BLOCKS --num-ranges 1 --count 1 --threads 1 --queue-depth 1 2>&1 | head -50
echo ""

echo "=== xcopy_tool ioctl structure (from strace) ==="
grep -A 20 "NVME_IOCTL_IO_CMD" /tmp/xcopy_ioctl.log | head -30
echo ""

echo "=== Comparison ==="
echo "Compare the ioctl structure dumps above to identify differences"
echo ""

