#!/bin/bash
# Compare exact ioctl structure bytes between nvme-cli and our tool

DEVICE="/dev/nvme1n1"
SRC_LBA=0
DST_LBA=1937768448
BLOCKS=8192

echo "=== Comparing ioctl Structure Bytes ==="
echo ""

# Write test data
echo "TEST_BYTES_$(date +%s)" | sudo dd of="$DEVICE" bs=512 seek=$SRC_LBA count=$BLOCKS 2>/dev/null

echo "=== nvme-cli ioctl structure (first 128 bytes) ==="
sudo strace -e trace=ioctl -s 2000 -xx nvme copy "$DEVICE" --sdlba=$SRC_LBA --blocks=$((BLOCKS-1)) --slbs=$DST_LBA 2>&1 | grep -A 30 "NVME_IOCTL_IO_CMD" | grep -E "0x[0-9a-f]{16}" | head -8
echo ""

echo "=== xcopy_tool ioctl structure (first 128 bytes) ==="
sudo strace -e trace=ioctl -s 2000 -xx -f -o /tmp/xcopy_bytes.log ./xcopy_tool --src-device "$DEVICE" --dst-device "$DEVICE" --src-lba $SRC_LBA --dst-lba $DST_LBA --range-size $BLOCKS --num-ranges 1 --count 1 --threads 1 --queue-depth 1 2>&1 | head -30
echo ""

echo "=== xcopy_tool ioctl bytes from strace ==="
grep -A 30 "NVME_IOCTL_IO_CMD" /tmp/xcopy_bytes.log | grep -E "0x[0-9a-f]{16}" | head -8
echo ""

echo "=== Side-by-side comparison ==="
echo "Extract the hex dumps above and compare byte-by-byte"
echo ""

