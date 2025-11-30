#include "xcopy_cmd.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <endian.h>

int xcopy_cmd_build(struct xcopy_operation *op,
                    uint32_t dst_nsid,
                    struct copy_range_descriptor *ranges,
                    uint32_t num_ranges) {
    if (!op || !ranges || num_ranges == 0 || num_ranges > MAX_COPY_RANGES) {
        return -EINVAL;
    }
    
    // Clear command structure
    memset(&op->cmd, 0, sizeof(op->cmd));
    
    // Set command opcode (NVMe Copy = 0x19)
    op->cmd.opcode = NVME_OPC_COPY;
    
    // Set destination namespace ID
    op->cmd.nsid = dst_nsid;
    op->dst_nsid = dst_nsid;
    
    // Set number of ranges (0-based, so num_ranges-1)
    op->cmd.cdw10 = (num_ranges - 1) & 0xFFFF;
    
    // Set flags and data for passthrough command
    // Note: libnvme's nvme_passthru_cmd structure fields may vary by version
    // Common fields: opcode, flags, rsvd1, nsid, cdw2-cdw15, data_len, metadata_len
    // Data pointer is typically passed separately to nvme_submit_io_passthru()
    op->cmd.flags = 0;  // No special flags
    op->cmd.rsvd1 = 0;
    op->cmd.data_len = xcopy_cmd_get_data_size(num_ranges);
    op->cmd.metadata_len = 0;
    op->cmd.timeout_ms = 0;  // Use default timeout
    // Note: Data pointer (op->ranges) is passed separately to submit function
    
    // Copy range descriptors and convert to little-endian (NVMe spec requirement)
    // Debug: Print first range descriptor (only once)
    static int range_debug_logged = 0;
    if (!range_debug_logged && num_ranges > 0) {
        fprintf(stderr, "DEBUG: First range descriptor before conversion: src_nsid=%u, src_lba=%lu, num_blocks=%u, dst_lba=%lu\n",
                ranges[0].src_nsid, ranges[0].src_lba, ranges[0].num_blocks, ranges[0].dst_lba);
        range_debug_logged = 1;
    }
    
    for (uint32_t i = 0; i < num_ranges; i++) {
        op->ranges[i].rsvd0 = htole32(ranges[i].rsvd0);
        op->ranges[i].src_nsid = htole32(ranges[i].src_nsid);
        op->ranges[i].src_lba = htole64(ranges[i].src_lba);
        op->ranges[i].rsvd1 = htole32(ranges[i].rsvd1);
        op->ranges[i].num_blocks = htole32(ranges[i].num_blocks);
        op->ranges[i].dst_lba = htole64(ranges[i].dst_lba);
        // Note: No rsvd2/rsvd3 - descriptor is exactly 32 bytes per NVMe spec
    }
    op->num_ranges = num_ranges;
    
    // Initialize operation state
    op->completed = false;
    op->status = 0;
    op->start_time_us = 0;
    op->user_data = NULL;
    
    return 0;
}

bool xcopy_cmd_validate_range(struct copy_range_descriptor *range,
                              uint32_t src_nsid,
                              uint64_t namespace_size) {
    if (!range) {
        return false;
    }
    
    // Validate namespace ID matches
    if (range->src_nsid != src_nsid) {
        return false;
    }
    
    // Validate number of blocks (0-based, so 0 = 1 block)
    // Maximum is 0xFFFFFFFF which means 0x100000000 blocks
    // But we'll use a more reasonable limit
    if (range->num_blocks > 0x1000000) { // 16M blocks max
        return false;
    }
    
    // Calculate actual number of blocks (num_blocks is 0-based)
    uint64_t actual_blocks = (uint64_t)range->num_blocks + 1;
    
    // Validate source LBA range
    if (range->src_lba + actual_blocks > namespace_size) {
        return false;
    }
    
    // Validate destination LBA range
    if (range->dst_lba + actual_blocks > namespace_size) {
        return false;
    }
    
    return true;
}

size_t xcopy_cmd_get_data_size(uint32_t num_ranges) {
    // Each range descriptor is 32 bytes
    // The command data contains the range descriptors
    return num_ranges * sizeof(struct copy_range_descriptor);
}

