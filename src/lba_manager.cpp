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
    if (dst_end_ > namespace_size_) {
        dst_end_ = namespace_size_;
    }
}

// LOCK-FREE implementation using compare-exchange
uint64_t LbaManager::get_and_advance_dst_lba(uint64_t total_blocks) {
    if (total_blocks == 0) {
        return dst_current_.load(std::memory_order_relaxed);
    }
    
    uint64_t range_size = dst_end_ - dst_start_;
    
    // Lock-free atomic advance with wrap-around
    uint64_t current, next, result;
    do {
        current = dst_current_.load(std::memory_order_relaxed);
        
        // Check if operation would exceed bounds
        if (current + total_blocks > dst_end_) {
            // Wrap to start
            result = dst_start_;
            next = dst_start_ + total_blocks;
            if (total_blocks > range_size) {
                next = dst_start_; // Operation too large, just reset
            }
        } else {
            result = current;
            next = current + total_blocks;
        }
        
        // Handle wrap for next position
        if (next >= dst_end_) {
            next = dst_start_;
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
    
    // Lock-free atomic advance
    uint64_t current, next;
    do {
        current = dst_current_.load(std::memory_order_relaxed);
        next = current + total_blocks;
        if (next >= dst_end_) {
            next = dst_start_;
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
    dst_current_.store(dst_start_, std::memory_order_release);
}

void LbaManager::set_dst_range(uint64_t start, uint64_t end) {
    dst_start_ = start;
    dst_end_ = (end == 0 || end > namespace_size_) ? namespace_size_ : end;
    dst_current_.store(dst_start_, std::memory_order_release);
}

uint64_t LbaManager::get_dst_range_size() const {
    if (dst_end_ > dst_start_) {
        return dst_end_ - dst_start_;
    }
    return namespace_size_ - dst_start_;
}

} // namespace xload

