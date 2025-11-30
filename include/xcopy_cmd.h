#ifndef XCOPY_CMD_H
#define XCOPY_CMD_H

#include <stdint.h>
#include <stdbool.h>
#include <spdk/nvme.h>

// Maximum number of ranges per NVMe Copy command
#define MAX_COPY_RANGES 16

// NVMe Copy command opcode
#define NVME_OPC_COPY 0x19

// Copy Range Descriptor structure (32 bytes as per NVMe spec)
struct copy_range_descriptor {
    uint32_t rsvd0;
    uint32_t src_nsid;
    uint64_t src_lba;
    uint32_t rsvd1;
    uint32_t num_blocks;       // Number of logical blocks (0-based, so 0 = 1 block)
    uint64_t dst_lba;
    uint32_t rsvd2;
    uint32_t rsvd3;
} __attribute__((packed));

// XCOPY operation context
struct xcopy_operation {
    struct spdk_nvme_cmd cmd;
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

