#!/bin/bash
# Verification script for XCOPY operations
# This script helps verify that XCOPY commands actually copied data

set -e

DEVICE="/dev/nvme1n1"
SRC_LBA=0
DST_LBA=1937768448
BLOCK_SIZE=512
NUM_BLOCKS=8192  # 8 ranges * 1024 blocks per range

echo "=== XCOPY Verification Script ==="
echo ""

# Method 1: Read source data and compare with destination
echo "Method 1: Comparing source and destination data"
echo "Reading source data from LBA $SRC_LBA..."
dd if=$DEVICE of=/tmp/src_data.bin bs=$BLOCK_SIZE skip=$SRC_LBA count=$NUM_BLOCKS 2>/dev/null

echo "Reading destination data from LBA $DST_LBA..."
dd if=$DEVICE of=/tmp/dst_data.bin bs=$BLOCK_SIZE skip=$DST_LBA count=$NUM_BLOCKS 2>/dev/null

echo "Comparing data..."
if cmp -s /tmp/src_data.bin /tmp/dst_data.bin; then
    echo "✓ SUCCESS: Source and destination data match!"
else
    echo "✗ FAIL: Source and destination data differ"
    echo "Showing first differences:"
    cmp -l /tmp/src_data.bin /tmp/dst_data.bin | head -20
fi

echo ""

# Method 2: Check data integrity with checksums
echo "Method 2: Checksum verification"
SRC_CHECKSUM=$(md5sum /tmp/src_data.bin | cut -d' ' -f1)
DST_CHECKSUM=$(md5sum /tmp/dst_data.bin | cut -d' ' -f1)

echo "Source checksum:  $SRC_CHECKSUM"
echo "Destination checksum: $DST_CHECKSUM"

if [ "$SRC_CHECKSUM" == "$DST_CHECKSUM" ]; then
    echo "✓ SUCCESS: Checksums match!"
else
    echo "✗ FAIL: Checksums differ"
fi

echo ""

# Method 3: Verify specific pattern (if you wrote known data first)
echo "Method 3: Pattern verification"
echo "Note: This requires writing known data to source first"
echo "You can write a pattern like:"
echo "  sudo nvme write $DEVICE --data-size=$((BLOCK_SIZE * NUM_BLOCKS)) --data=/tmp/pattern.bin --start-block=$SRC_LBA"
echo ""

# Cleanup
rm -f /tmp/src_data.bin /tmp/dst_data.bin

echo "=== Verification Complete ==="

