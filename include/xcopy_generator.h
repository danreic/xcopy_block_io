#ifndef XCOPY_GENERATOR_H
#define XCOPY_GENERATOR_H

#include <spdk/nvme.h>
#include <stdint.h>
#include <vector>
#include <random>
#include "lba_manager.h"

namespace xload {

struct XcopyOperation {
    uint32_t dst_nsid;
    uint64_t dst_lba;
    uint32_t num_ranges;
    struct spdk_nvme_scc_source_range* ranges;  // Allocated via SPDK DMA
    uint64_t total_blocks;
    uint64_t start_time_ns;
    void* user_data;  // For callback context
    
    XcopyOperation();
    ~XcopyOperation();
    
    // Copy constructor
    XcopyOperation(const XcopyOperation& other);
    XcopyOperation& operator=(const XcopyOperation& other);
    
    // Allocate ranges buffer
    int allocate_ranges(uint32_t max_ranges);
    
    // Free ranges buffer
    void free_ranges();
};

class XcopyGenerator {
public:
    XcopyGenerator(uint32_t max_ranges, 
                   const std::vector<uint32_t>& src_nsids,
                   uint32_t dst_nsid,
                   const std::vector<const struct spdk_nvme_ns*>& namespaces,
                   bool enable_cross_namespace = false,
                   bool target_supports_cross_namespace = false);
    
    // Generate next XCOPY operation with randomized num_ranges
    int generate(XcopyOperation& op, LbaManager& lba_mgr, uint64_t range_size);
    
    // Get random number of ranges (1 to max_ranges)
    uint32_t get_random_num_ranges() { return range_dist_(rng_); }
    
    uint32_t max_ranges_;
    
private:
    std::vector<uint32_t> src_nsids_;
    uint32_t dst_nsid_;
    std::vector<const struct spdk_nvme_ns*> namespaces_;
    bool enable_cross_namespace_;
    bool target_supports_cross_namespace_;
    std::mt19937_64 rng_;
    std::uniform_int_distribution<uint32_t> range_dist_;
    
    // Get random source NSID
    uint32_t get_random_src_nsid();
    
    // Get namespace size
    uint64_t get_ns_size(uint32_t nsid);
};

} // namespace xload

#endif // XCOPY_GENERATOR_H

