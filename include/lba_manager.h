#ifndef LBA_MANAGER_H
#define LBA_MANAGER_H

#include <stdint.h>
#include <random>
#include <atomic>

namespace xload {

class LbaManager {
public:
    LbaManager(uint64_t namespace_size, uint64_t start_lba = 0, uint64_t end_lba = 0);
    
    // Get next destination LBA and reserve space for it (LOCK-FREE atomic operation)
    // This atomically returns the current LBA and advances by total_blocks
    // This prevents race conditions when multiple threads are generating operations
    uint64_t get_and_advance_dst_lba(uint64_t total_blocks);
    
    // Get next destination LBA (sequential cycling)
    // Note: This returns the current LBA but does NOT advance it
    // DEPRECATED: Use get_and_advance_dst_lba() instead for thread safety
    uint64_t get_next_dst_lba(uint64_t range_size);
    
    // Advance destination LBA by specified number of blocks
    // DEPRECATED: Use get_and_advance_dst_lba() instead for thread safety
    void advance_dst_lba(uint64_t total_blocks);
    
    // Get random source LBA within namespace bounds (uses thread-local RNG)
    uint64_t get_random_src_lba(uint64_t range_size);
    
    // Reset destination LBA counter
    void reset_dst_lba();
    
    // Set destination LBA range
    // NOTE: This method is thread-safe but should ideally be called before threads start
    // or during a pause in I/O generation for predictable behavior
    void set_dst_range(uint64_t start, uint64_t end);
    
    // Get namespace size
    uint64_t get_namespace_size() const { return namespace_size_; }
    
    uint64_t get_dst_range_size() const;
    
private:
    uint64_t namespace_size_;
    std::atomic<uint64_t> dst_start_;   // Atomic to allow safe set_dst_range() during operation
    std::atomic<uint64_t> dst_end_;     // Atomic to allow safe set_dst_range() during operation
    std::atomic<uint64_t> dst_current_; // Lock-free atomic for high performance
};

} // namespace xload

#endif // LBA_MANAGER_H

