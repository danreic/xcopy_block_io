#!/bin/bash
# Test XCOPY with a single range (like nvme-cli does)

DEVICE="/dev/nvme1n1"
SRC_LBA=0
DST_LBA=1937768448
BLOCKS=8192

echo "Testing XCOPY with single range (matching nvme-cli)..."
echo "Device: $DEVICE"
echo "Source LBA: $SRC_LBA"
echo "Destination LBA: $DST_LBA"
echo "Blocks: $BLOCKS"
echo ""

# Generate test data
echo "Step 1: Generating test data..."
openssl rand $((BLOCKS * 512)) > /tmp/test_single.bin 2>/dev/null || \
head -c $((BLOCKS * 512)) /dev/urandom > /tmp/test_single.bin

# Write to source
echo "Step 2: Writing test data to source..."
sudo dd if=/tmp/test_single.bin of="$DEVICE" bs=512 seek=$SRC_LBA count=$BLOCKS conv=fsync 2>/dev/null

# Run XCOPY with single range
echo "Step 3: Running XCOPY with single range..."
sudo ./xcopy_tool \
  --src-device "$DEVICE" \
  --dst-device "$DEVICE" \
  --src-lba $SRC_LBA \
  --dst-lba $DST_LBA \
  --range-size $BLOCKS \
  --num-ranges 1 \
  --count 1 \
  --threads 1 \
  --queue-depth 1 \
  -v

if [ $? -eq 0 ]; then
    echo ""
    echo "Step 4: Reading and comparing..."
    sudo dd if="$DEVICE" of=/tmp/test_src_read.bin bs=512 skip=$SRC_LBA count=$BLOCKS 2>/dev/null
    sudo dd if="$DEVICE" of=/tmp/test_dst_read.bin bs=512 skip=$DST_LBA count=$BLOCKS 2>/dev/null
    
    if cmp -s /tmp/test_src_read.bin /tmp/test_dst_read.bin; then
        echo "✓ SUCCESS: Single range XCOPY works!"
    else
        echo "✗ FAIL: Data doesn't match"
        echo "  Source MD5: $(md5sum /tmp/test_src_read.bin | cut -d' ' -f1)"
        echo "  Dest MD5:   $(md5sum /tmp/test_dst_read.bin | cut -d' ' -f1)"
    fi
    
    rm -f /tmp/test_src_read.bin /tmp/test_dst_read.bin
fi

rm -f /tmp/test_single.bin

