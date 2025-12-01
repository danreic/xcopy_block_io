#include "xcopy_cmd.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <endian.h>
// libnvme.h is already included via xcopy_cmd.h

int xcopy_cmd_build(struct xcopy_operation *op,
                    uint32_t dst_nsid,
                    struct copy_range_descriptor *ranges,
                    uint32_t num_ranges) {
    if (!op || !ranges || num_ranges == 0 || num_ranges > MAX_COPY_RANGES) {
        return -EINVAL;
    }
    
    // Clear command structure
    memset(&op->cmd, 0, sizeof(op->cmd));
    
    op->dst_nsid = dst_nsid;
    op->num_ranges = num_ranges;
    
    // Prepare arrays for libnvme's nvme_init_copy_range_f2() function
    // Format 2 is for cross-namespace copy (includes src_nsid)
    __u32 snsids[MAX_COPY_RANGES];
    __u16 nlbs[MAX_COPY_RANGES];
    __u64 slbas[MAX_COPY_RANGES];
    __u16 sopts[MAX_COPY_RANGES] = {0};  // Source options (all zeros for now)
    __u32 eilbrts_short[MAX_COPY_RANGES] = {0};  // Expected LBA reference tags (short format)
    __u32 elbatms[MAX_COPY_RANGES] = {0};  // Expected LBA application tag masks
    __u32 elbats[MAX_COPY_RANGES] = {0};   // Expected LBA application tags
    
    // Extract data from our range descriptors
    // Debug: Print first range descriptor (only once)
    static int range_debug_logged = 0;
    if (!range_debug_logged && num_ranges > 0) {
        fprintf(stderr, "DEBUG: First range descriptor before conversion: src_nsid=%u, src_lba=%lu, num_blocks=%u, dst_lba=%lu\n",
                ranges[0].src_nsid, ranges[0].src_lba, ranges[0].num_blocks, ranges[0].dst_lba);
        range_debug_logged = 1;
    }
    
    // Calculate destination LBA from first range (all ranges should have same dst_lba pattern)
    // For simplicity, we'll use the first range's dst_lba as the base
    // In practice, ranges should be consecutive in destination
    uint64_t sdlba = ranges[0].dst_lba;
    
    for (uint32_t i = 0; i < num_ranges; i++) {
        snsids[i] = ranges[i].src_nsid;
        slbas[i] = ranges[i].src_lba;
        nlbs[i] = ranges[i].num_blocks;  // Already 0-based
        // Note: We're using format 2 which supports cross-namespace copy
    }
    
    // Use libnvme's nvme_init_copy_range_f2() to build range descriptors (format 2)
    // This matches nvme-cli's approach for cross-namespace copy
    // We need to use the correct structure type for format 2
    struct nvme_copy_range_f2 *f2_ranges = (struct nvme_copy_range_f2 *)op->ranges;
    nvme_init_copy_range_f2(f2_ranges, 
                            snsids, nlbs, slbas, sopts,
                            eilbrts_short, elbatms, elbats, num_ranges);
    
    // Build the NVMe Copy command structure manually
    // Set command opcode (NVMe Copy = 0x19)
    op->cmd.opcode = NVME_OPC_COPY;
    
    // Set destination namespace ID
    op->cmd.nsid = dst_nsid;
    
    // Set number of ranges (0-based, so num_ranges-1) in CDW10
    // Format 2 = cross-namespace copy (bits 31:16 = format, bits 15:0 = num_ranges-1)
    op->cmd.cdw10 = ((2 << 16) | ((num_ranges - 1) & 0xFFFF));
    
    // Set destination starting LBA in CDW11 and CDW12
    op->cmd.cdw11 = (uint32_t)(sdlba & 0xFFFFFFFF);
    op->cmd.cdw12 = (uint32_t)((sdlba >> 32) & 0xFFFFFFFF);
    
    // Set flags and data for passthrough command
    op->cmd.flags = 0;  // No special flags
    op->cmd.rsvd1 = 0;  // Reserved field - kernel will manage command_id
    op->cmd.data_len = xcopy_cmd_get_data_size(num_ranges);
    op->cmd.metadata_len = 0;
    op->cmd.timeout_ms = 60000;  // 60 second timeout for NVMe-TCP
    op->cmd.addr = (__u64)(uintptr_t)op->ranges;
    
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
    // Each range descriptor is 32 bytes (format 2: struct nvme_copy_range_f2)
    // The command data contains the range descriptors
    return num_ranges * sizeof(struct nvme_copy_range_f2);
}

