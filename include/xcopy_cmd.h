#ifndef XCOPY_CMD_H
#define XCOPY_CMD_H

#include <stdint.h>
#include <stdbool.h>
#include <libnvme.h>

// Maximum number of ranges per NVMe Copy command
#define MAX_COPY_RANGES 16

// NVMe Copy command opcode
#define NVME_OPC_COPY 0x19

// Copy Range Descriptor structure (32 bytes as per NVMe spec)
// Format: DWORD 0: Reserved, DWORD 1: Source NSID, DWORD 2-3: Source LBA,
//         DWORD 4: Reserved, DWORD 5: Number of Blocks, DWORD 6-7: Destination LBA
struct copy_range_descriptor {
    uint32_t rsvd0;            // DWORD 0: Reserved
    uint32_t src_nsid;         // DWORD 1: Source Namespace Identifier
    uint64_t src_lba;          // DWORD 2-3: Source Starting LBA
    uint32_t rsvd1;            // DWORD 4: Reserved
    uint32_t num_blocks;       // DWORD 5: Number of logical blocks (0-based, so 0 = 1 block)
    uint64_t dst_lba;          // DWORD 6-7: Destination Starting LBA
    // Total: 4+4+8+4+4+8 = 32 bytes (no additional reserved fields)
} __attribute__((packed));

// XCOPY operation context
struct xcopy_operation {
    struct nvme_passthru_cmd cmd;  // libnvme command structure
    struct copy_range_descriptor ranges[MAX_COPY_RANGES];
    uint32_t num_ranges;
    uint32_t dst_nsid;
    void *user_data;
    uint64_t start_time_us;
    bool completed;
    int status;
};

// Build NVMe Copy command
int xcopy_cmd_build(struct xcopy_operation *op,
                    uint32_t dst_nsid,
                    struct copy_range_descriptor *ranges,
                    uint32_t num_ranges);

// Validate copy range descriptor
bool xcopy_cmd_validate_range(struct copy_range_descriptor *range,
                              uint32_t src_nsid,
                              uint64_t namespace_size);

// Get required data buffer size for command
size_t xcopy_cmd_get_data_size(uint32_t num_ranges);

#endif // XCOPY_CMD_H

