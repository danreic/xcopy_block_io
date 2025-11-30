#ifndef RANGE_MANAGER_H
#define RANGE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "xcopy_cmd.h"

// Range workload configuration
struct range_workload {
    uint64_t src_lba_start;
    uint64_t dst_lba_start;
    uint64_t range_size;       // Size in blocks
    uint32_t num_ranges;
    uint32_t src_nsid;
    uint32_t dst_nsid;
    uint64_t namespace_size;   // Size of namespace in blocks
};

// Range descriptor generator
struct range_generator {
    struct range_workload workload;
    uint32_t current_range;
    uint64_t current_src_lba;
    uint64_t current_dst_lba;
};

// Initialize range generator
int range_generator_init(struct range_generator *gen,
                        struct range_workload *workload);

// Get next set of ranges for a copy command
int range_generator_next(struct range_generator *gen,
                        struct copy_range_descriptor *ranges,
                        uint32_t max_ranges,
                        uint32_t *num_ranges);

// Reset generator to start
void range_generator_reset(struct range_generator *gen);

// Check if generator has more ranges
bool range_generator_has_more(struct range_generator *gen);

#endif // RANGE_MANAGER_H

