#include "lba_manager.h"
#include <random>
#include <chrono>
#include <pthread.h>

namespace xload {

// Thread-local RNG for lock-free random number generation
static thread_local std::mt19937_64 tl_rng(
    std::chrono::steady_clock::now().time_since_epoch().count() ^ 
    reinterpret_cast<uint64_t>(pthread_self()));

LbaManager::LbaManager(uint64_t namespace_size, uint64_t start_lba, uint64_t end_lba)
    : namespace_size_(namespace_size)
    , dst_start_(start_lba)
    , dst_end_(end_lba == 0 ? namespace_size : end_lba)
    , dst_current_(start_lba)
{
    uint64_t end = dst_end_.load(std::memory_order_relaxed);
    if (end > namespace_size_) {
        dst_end_.store(namespace_size_, std::memory_order_relaxed);
    }
}

// LOCK-FREE implementation using compare-exchange
uint64_t LbaManager::get_and_advance_dst_lba(uint64_t total_blocks) {
    if (total_blocks == 0) {
        return dst_current_.load(std::memory_order_relaxed);
    }
    
    // Read atomic bounds once per operation for consistency
    uint64_t start = dst_start_.load(std::memory_order_acquire);
    uint64_t end = dst_end_.load(std::memory_order_acquire);
    uint64_t range_size = end - start;
    
    // Lock-free atomic advance with wrap-around
    uint64_t current, next, result;
    do {
        current = dst_current_.load(std::memory_order_relaxed);
        
        // Check if operation would exceed bounds
        if (current + total_blocks > end) {
            // Wrap to start
            result = start;
            next = start + total_blocks;
            if (total_blocks > range_size) {
                next = start; // Operation too large, just reset
            }
        } else {
            result = current;
            next = current + total_blocks;
        }
        
        // Handle wrap for next position
        if (next >= end) {
            next = start;
        }
        
    } while (!dst_current_.compare_exchange_weak(current, next,
                std::memory_order_release, std::memory_order_relaxed));
    
    return result;
}

uint64_t LbaManager::get_next_dst_lba(uint64_t range_size) {
    (void)range_size;
    return dst_current_.load(std::memory_order_relaxed);
}

void LbaManager::advance_dst_lba(uint64_t total_blocks) {
    if (total_blocks == 0) {
        return;
    }
    
    // Read atomic bounds once for consistency
    uint64_t start = dst_start_.load(std::memory_order_acquire);
    uint64_t end = dst_end_.load(std::memory_order_acquire);
    
    // Lock-free atomic advance
    uint64_t current, next;
    do {
        current = dst_current_.load(std::memory_order_relaxed);
        next = current + total_blocks;
        if (next >= end) {
            next = start;
        }
    } while (!dst_current_.compare_exchange_weak(current, next,
                std::memory_order_release, std::memory_order_relaxed));
}

// Thread-local RNG - no locking needed
uint64_t LbaManager::get_random_src_lba(uint64_t range_size) {
    if (range_size >= namespace_size_) {
        return 0;
    }
    
    // Use 90% of namespace to avoid edge cases
    uint64_t safe_namespace_size = (namespace_size_ * 90) / 100;
    if (safe_namespace_size < range_size) {
        safe_namespace_size = namespace_size_;
    }
    
    uint64_t max_lba = safe_namespace_size - range_size;
    
    // Use thread-local RNG - no lock needed
    return tl_rng() % (max_lba + 1);
}

void LbaManager::reset_dst_lba() {
    dst_current_.store(dst_start_.load(std::memory_order_acquire), std::memory_order_release);
}

void LbaManager::set_dst_range(uint64_t start, uint64_t end) {
    uint64_t new_end = (end == 0 || end > namespace_size_) ? namespace_size_ : end;
    // Update end first, then start, then current - this ordering ensures
    // that readers see consistent bounds even during concurrent updates
    dst_end_.store(new_end, std::memory_order_release);
    dst_start_.store(start, std::memory_order_release);
    dst_current_.store(start, std::memory_order_release);
}

uint64_t LbaManager::get_dst_range_size() const {
    uint64_t start = dst_start_.load(std::memory_order_acquire);
    uint64_t end = dst_end_.load(std::memory_order_acquire);
    if (end > start) {
        return end - start;
    }
    return namespace_size_ - start;
}

} // namespace xload

