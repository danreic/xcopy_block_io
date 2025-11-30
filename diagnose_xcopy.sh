#!/bin/bash
# Diagnostic script to test XCOPY with different scenarios

DEVICE="/dev/nvme1n1"
BLOCKS=1024  # Smaller test - 512KB

echo "=== XCOPY Diagnostic Tests ==="
echo ""

# Test 1: Try copying to a different, closer destination LBA
echo "Test 1: Copying to a closer destination LBA (1000000)..."
SRC_LBA=0
DST_LBA=1000000

# Write test data
echo "TEST1_$(date +%s)" | sudo dd of="$DEVICE" bs=512 seek=$SRC_LBA count=$BLOCKS 2>/dev/null

# Read source
sudo dd if="$DEVICE" of=/tmp/test1_src.bin bs=512 skip=$SRC_LBA count=$BLOCKS 2>/dev/null

# Try XCOPY
sudo nvme copy "$DEVICE" --sdlba=$SRC_LBA --blocks=$((BLOCKS-1)) --slbs=$DST_LBA 2>&1

# Read destination
sudo dd if="$DEVICE" of=/tmp/test1_dst.bin bs=512 skip=$DST_LBA count=$BLOCKS 2>/dev/null

if cmp -s /tmp/test1_src.bin /tmp/test1_dst.bin; then
    echo "✓ Test 1 PASSED: Data copied successfully to LBA $DST_LBA"
else
    echo "✗ Test 1 FAILED: Data doesn't match at LBA $DST_LBA"
    echo "  Source MD5: $(md5sum /tmp/test1_src.bin | cut -d' ' -f1)"
    echo "  Dest MD5:   $(md5sum /tmp/test1_dst.bin | cut -d' ' -f1)"
fi
rm -f /tmp/test1_src.bin /tmp/test1_dst.bin
echo ""

# Test 2: Try with a single range (simpler command)
echo "Test 2: Single range copy (1 block)..."
SRC_LBA=0
DST_LBA=2000000

# Write single block
echo "TEST2_SINGLE" | sudo dd of="$DEVICE" bs=512 seek=$SRC_LBA count=1 2>/dev/null

# Read source
sudo dd if="$DEVICE" of=/tmp/test2_src.bin bs=512 skip=$SRC_LBA count=1 2>/dev/null

# Try XCOPY with single block
sudo nvme copy "$DEVICE" --sdlba=$SRC_LBA --blocks=0 --slbs=$DST_LBA 2>&1

# Read destination
sudo dd if="$DEVICE" of=/tmp/test2_dst.bin bs=512 skip=$DST_LBA count=1 2>/dev/null

if cmp -s /tmp/test2_src.bin /tmp/test2_dst.bin; then
    echo "✓ Test 2 PASSED: Single block copied successfully"
else
    echo "✗ Test 2 FAILED: Single block copy didn't work"
fi
rm -f /tmp/test2_src.bin /tmp/test2_dst.bin
echo ""

# Test 3: Check if we can write directly to destination
echo "Test 3: Verifying destination is writable..."
DST_LBA=3000000
echo "DIRECT_WRITE_TEST" | sudo dd of="$DEVICE" bs=512 seek=$DST_LBA count=1 2>/dev/null
sudo dd if="$DEVICE" of=/tmp/test3_read.bin bs=512 skip=$DST_LBA count=1 2>/dev/null

if grep -q "DIRECT_WRITE_TEST" /tmp/test3_read.bin 2>/dev/null; then
    echo "✓ Test 3 PASSED: Destination LBA is writable"
else
    echo "✗ Test 3 FAILED: Cannot write directly to destination (might be read-only)"
fi
rm -f /tmp/test3_read.bin
echo ""

echo "=== Diagnostic Complete ==="
echo ""
echo "If all tests fail, this suggests a server-side XCOPY implementation issue."
echo "Check server logs for any errors or warnings related to XCOPY operations."

