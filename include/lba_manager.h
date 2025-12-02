#ifndef LBA_MANAGER_H
#define LBA_MANAGER_H

#include <stdint.h>
#include <random>

namespace xload {

class LbaManager {
public:
    LbaManager(uint64_t namespace_size, uint64_t start_lba = 0, uint64_t end_lba = 0);
    
    // Get next destination LBA (sequential cycling)
    uint64_t get_next_dst_lba(uint64_t range_size);
    
    // Get random source LBA within namespace bounds
    uint64_t get_random_src_lba(uint64_t range_size);
    
    // Reset destination LBA counter
    void reset_dst_lba();
    
    // Set destination LBA range
    void set_dst_range(uint64_t start, uint64_t end);
    
private:
    uint64_t namespace_size_;
    uint64_t dst_start_;
    uint64_t dst_end_;
    uint64_t dst_current_;
    std::mt19937_64 rng_;
    
    uint64_t get_dst_range_size() const;
};

} // namespace xload

#endif // LBA_MANAGER_H

