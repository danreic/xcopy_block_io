#ifndef OPERATION_POOL_H
#define OPERATION_POOL_H

#include <spdk/nvme.h>
#include <spdk/env.h>
#include <atomic>
#include <vector>
#include <cstring>

namespace xload {

// Pre-allocated operation with DMA buffer
struct PooledOperation {
    uint32_t dst_nsid;
    uint64_t dst_lba;
    uint32_t num_ranges;
    uint32_t max_ranges;
    struct spdk_nvme_scc_source_range* ranges;  // Pre-allocated DMA buffer
    uint64_t total_blocks;
    uint64_t start_time_ns;
    void* user_data;
    
    // Pool management
    std::atomic<bool> in_use;
    uint32_t pool_index;
    
    PooledOperation() 
        : dst_nsid(0), dst_lba(0), num_ranges(0), max_ranges(0)
        , ranges(nullptr), total_blocks(0), start_time_ns(0)
        , user_data(nullptr), in_use(false), pool_index(0) {}
    
    void reset() {
        dst_nsid = 0;
        dst_lba = 0;
        num_ranges = 0;
        total_blocks = 0;
        start_time_ns = 0;
        user_data = nullptr;
        if (ranges && max_ranges > 0) {
            memset(ranges, 0, max_ranges * sizeof(struct spdk_nvme_scc_source_range));
        }
    }
};

// Lock-free operation pool using atomic operations
class OperationPool {
public:
    OperationPool() : pool_size_(0), operations_(nullptr), next_index_(0) {}
    
    ~OperationPool() {
        cleanup();
    }
    
    // Initialize pool with given size and max_ranges per operation
    int init(uint32_t pool_size, uint32_t max_ranges) {
        if (operations_) {
            return 0; // Already initialized
        }
        
        pool_size_ = pool_size;
        max_ranges_ = max_ranges;
        operations_ = new PooledOperation[pool_size];
        
        // Pre-allocate DMA buffers for all operations
        for (uint32_t i = 0; i < pool_size; i++) {
            operations_[i].pool_index = i;
            operations_[i].max_ranges = max_ranges;
            operations_[i].in_use = false;
            
            size_t dma_size = max_ranges * sizeof(struct spdk_nvme_scc_source_range);
            operations_[i].ranges = static_cast<struct spdk_nvme_scc_source_range*>(
                spdk_dma_malloc(dma_size, 64, nullptr));
            
            if (!operations_[i].ranges) {
                // Cleanup on failure
                cleanup();
                return -1;
            }
            memset(operations_[i].ranges, 0, dma_size);
        }
        
        return 0;
    }
    
    // Acquire an operation from pool (lock-free)
    PooledOperation* acquire() {
        // Try multiple slots to find a free one
        for (uint32_t attempts = 0; attempts < pool_size_; attempts++) {
            uint32_t idx = next_index_.fetch_add(1, std::memory_order_relaxed) % pool_size_;
            
            bool expected = false;
            if (operations_[idx].in_use.compare_exchange_strong(
                    expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
                operations_[idx].reset();
                return &operations_[idx];
            }
        }
        return nullptr; // Pool exhausted
    }
    
    // Release operation back to pool (lock-free)
    void release(PooledOperation* op) {
        if (op && op->pool_index < pool_size_) {
            op->in_use.store(false, std::memory_order_release);
        }
    }
    
    uint32_t get_pool_size() const { return pool_size_; }
    
private:
    void cleanup() {
        if (operations_) {
            for (uint32_t i = 0; i < pool_size_; i++) {
                if (operations_[i].ranges) {
                    spdk_dma_free(operations_[i].ranges);
                    operations_[i].ranges = nullptr;
                }
            }
            delete[] operations_;
            operations_ = nullptr;
        }
        pool_size_ = 0;
    }
    
    uint32_t pool_size_;
    uint32_t max_ranges_;
    PooledOperation* operations_;
    std::atomic<uint32_t> next_index_;
};

} // namespace xload

#endif // OPERATION_POOL_H
