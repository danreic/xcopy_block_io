#include "range_manager.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

int range_generator_init(struct range_generator *gen,
                        struct range_workload *workload) {
    if (!gen || !workload) {
        return -EINVAL;
    }
    
    if (workload->num_ranges == 0 || workload->range_size == 0) {
        return -EINVAL;
    }
    
    // Copy workload configuration
    memcpy(&gen->workload, workload, sizeof(*workload));
    
    // Initialize generator state
    gen->current_range = 0;
    gen->current_src_lba = workload->src_lba_start;
    gen->current_dst_lba = workload->dst_lba_start;
    
    return 0;
}

int range_generator_next(struct range_generator *gen,
                        struct copy_range_descriptor *ranges,
                        uint32_t max_ranges,
                        uint32_t *num_ranges) {
    if (!gen || !ranges || !num_ranges || max_ranges == 0) {
        return -EINVAL;
    }
    
    if (gen->current_range >= gen->workload.num_ranges) {
        *num_ranges = 0;
        return 0; // No more ranges
    }
    
    // Determine how many ranges to generate in this batch
    uint32_t remaining = gen->workload.num_ranges - gen->current_range;
    uint32_t batch_size = (remaining < max_ranges) ? remaining : max_ranges;
    
    // Generate range descriptors
    for (uint32_t i = 0; i < batch_size; i++) {
        struct copy_range_descriptor *range = &ranges[i];
        
        memset(range, 0, sizeof(*range));
        
        // Set source namespace ID
        range->src_nsid = gen->workload.src_nsid;
        
        // Set source LBA
        range->src_lba = gen->current_src_lba;
        
        // Set number of blocks (0-based, so range_size-1)
        range->num_blocks = gen->workload.range_size - 1;
        
        // Set destination LBA
        range->dst_lba = gen->current_dst_lba;
        
        // Validate range doesn't exceed namespace size
        uint64_t actual_blocks = gen->workload.range_size;
        if (range->src_lba + actual_blocks > gen->workload.namespace_size) {
            return -ERANGE;
        }
        if (range->dst_lba + actual_blocks > gen->workload.namespace_size) {
            return -ERANGE;
        }
        
        // Advance to next range
        gen->current_src_lba += gen->workload.range_size;
        gen->current_dst_lba += gen->workload.range_size;
        gen->current_range++;
    }
    
    *num_ranges = batch_size;
    return 0;
}

void range_generator_reset(struct range_generator *gen) {
    if (!gen) {
        return;
    }
    
    gen->current_range = 0;
    gen->current_src_lba = gen->workload.src_lba_start;
    gen->current_dst_lba = gen->workload.dst_lba_start;
}

bool range_generator_has_more(struct range_generator *gen) {
    if (!gen) {
        return false;
    }
    return gen->current_range < gen->workload.num_ranges;
}

