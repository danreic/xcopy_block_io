#!/bin/bash
# Test script for XCOPY operations
# 1. Writes a known dataset to source location
# 2. Runs XCOPY tool to copy it
# 3. Verifies the copy by comparing source and destination

set -e

# Configuration
DEVICE="/dev/nvme1n1"
SRC_LBA=0
DST_LBA=1937768448
BLOCK_SIZE=512
NUM_RANGES=8
RANGE_SIZE=8192  # blocks per range
TOTAL_BLOCKS=$((NUM_RANGES * RANGE_SIZE))  # 8 * 8192 = 65536 blocks

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=========================================="
echo "XCOPY Verification Test"
echo "=========================================="
echo "Device: $DEVICE"
echo "Source LBA: $SRC_LBA"
echo "Destination LBA: $DST_LBA"
echo "Range size: $RANGE_SIZE blocks"
echo "Number of ranges: $NUM_RANGES"
echo "Total blocks: $TOTAL_BLOCKS ($(($TOTAL_BLOCKS * $BLOCK_SIZE / 1024 / 1024)) MB)"
echo ""

# Step 1: Generate test data with a known pattern
echo -e "${YELLOW}Step 1: Generating test data...${NC}"
TEST_DATA_FILE="/tmp/xcopy_test_data.bin"
TOTAL_SIZE=$((TOTAL_BLOCKS * BLOCK_SIZE))

# Create test data using a simple pattern
# Method 1: Use /dev/urandom for unique data (most reliable)
if command -v openssl >/dev/null 2>&1; then
    openssl rand $TOTAL_SIZE > "$TEST_DATA_FILE"
elif [ -c /dev/urandom ]; then
    head -c $TOTAL_SIZE /dev/urandom > "$TEST_DATA_FILE"
else
    # Fallback: create pattern data
    python3 << EOF 2>/dev/null || perl -e 'print chr(int(rand(256))) x '$TOTAL_SIZE > "$TEST_DATA_FILE" 2>/dev/null || \
    (echo "XCOPY_TEST_DATA" && head -c $((TOTAL_SIZE - 16)) /dev/zero) > "$TEST_DATA_FILE"
import os
with open('$TEST_DATA_FILE', 'wb') as f:
    f.write(os.urandom($TOTAL_SIZE))
EOF
fi

echo "Test data generated: $(ls -lh $TEST_DATA_FILE | awk '{print $5}')"
echo ""

# Step 2: Write test data to source location
echo -e "${YELLOW}Step 2: Writing test data to source (LBA $SRC_LBA)...${NC}"
sudo dd if="$TEST_DATA_FILE" of="$DEVICE" bs=$BLOCK_SIZE seek=$SRC_LBA count=$TOTAL_BLOCKS conv=fsync 2>/dev/null
if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Test data written successfully${NC}"
else
    echo -e "${RED}✗ Failed to write test data${NC}"
    exit 1
fi
echo ""

# Step 3: Verify source data was written correctly
echo -e "${YELLOW}Step 3: Verifying source data...${NC}"
sudo dd if="$DEVICE" of="/tmp/verify_source.bin" bs=$BLOCK_SIZE skip=$SRC_LBA count=$TOTAL_BLOCKS 2>/dev/null
if cmp -s "$TEST_DATA_FILE" "/tmp/verify_source.bin"; then
    echo -e "${GREEN}✓ Source data verified${NC}"
else
    echo -e "${RED}✗ Source data verification failed${NC}"
    exit 1
fi
echo ""

# Step 4: Run XCOPY tool
echo -e "${YELLOW}Step 4: Running XCOPY tool...${NC}"
echo "Command: ./xcopy_tool --src-device $DEVICE --dst-device $DEVICE \\"
echo "  --src-lba $SRC_LBA --dst-lba $DST_LBA \\"
echo "  --range-size $RANGE_SIZE --num-ranges $NUM_RANGES --count 1 --threads 1 --queue-depth 1"
echo ""

sudo ./xcopy_tool \
  --src-device "$DEVICE" \
  --dst-device "$DEVICE" \
  --src-lba $SRC_LBA \
  --dst-lba $DST_LBA \
  --range-size $RANGE_SIZE \
  --num-ranges $NUM_RANGES \
  --count 1 \
  --threads 1 \
  --queue-depth 1 \
  -v

XCOPY_EXIT=$?
if [ $XCOPY_EXIT -eq 0 ]; then
    echo -e "${GREEN}✓ XCOPY tool completed successfully${NC}"
else
    echo -e "${RED}✗ XCOPY tool failed with exit code $XCOPY_EXIT${NC}"
    exit 1
fi
echo ""

# Step 5: Read destination data
echo -e "${YELLOW}Step 5: Reading destination data (LBA $DST_LBA)...${NC}"
sudo dd if="$DEVICE" of="/tmp/verify_dest.bin" bs=$BLOCK_SIZE skip=$DST_LBA count=$TOTAL_BLOCKS 2>/dev/null
if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Destination data read successfully${NC}"
else
    echo -e "${RED}✗ Failed to read destination data${NC}"
    exit 1
fi
echo ""

# Step 6: Compare source and destination
echo -e "${YELLOW}Step 6: Comparing source and destination data...${NC}"
if cmp -s "$TEST_DATA_FILE" "/tmp/verify_dest.bin"; then
    echo -e "${GREEN}✓ SUCCESS: Source and destination data match perfectly!${NC}"
    echo ""
    echo "Verification details:"
    SRC_MD5=$(md5sum "$TEST_DATA_FILE" | cut -d' ' -f1)
    DST_MD5=$(md5sum "/tmp/verify_dest.bin" | cut -d' ' -f1)
    echo "  Source MD5:      $SRC_MD5"
    echo "  Destination MD5: $DST_MD5"
    echo ""
    echo -e "${GREEN}XCOPY operations are working correctly!${NC}"
    RESULT=0
else
    echo -e "${RED}✗ FAIL: Source and destination data differ${NC}"
    echo ""
    echo "Showing first differences:"
    cmp -l "$TEST_DATA_FILE" "/tmp/verify_dest.bin" | head -20
    echo ""
    SRC_MD5=$(md5sum "$TEST_DATA_FILE" | cut -d' ' -f1)
    DST_MD5=$(md5sum "/tmp/verify_dest.bin" | cut -d' ' -f1)
    echo "  Source MD5:      $SRC_MD5"
    echo "  Destination MD5: $DST_MD5"
    RESULT=1
fi
echo ""

# Cleanup
echo "Cleaning up temporary files..."
rm -f "$TEST_DATA_FILE" "/tmp/verify_source.bin" "/tmp/verify_dest.bin"
echo ""

if [ $RESULT -eq 0 ]; then
    echo "=========================================="
    echo -e "${GREEN}TEST PASSED${NC}"
    echo "=========================================="
    exit 0
else
    echo "=========================================="
    echo -e "${RED}TEST FAILED${NC}"
    echo "=========================================="
    exit 1
fi

