#include "lba_manager.h"
#include <random>
#include <chrono>

namespace xload {

LbaManager::LbaManager(uint64_t namespace_size, uint64_t start_lba, uint64_t end_lba)
    : namespace_size_(namespace_size)
    , dst_start_(start_lba)
    , dst_end_(end_lba == 0 ? namespace_size : end_lba)
    , dst_current_(start_lba)
    , rng_(std::chrono::steady_clock::now().time_since_epoch().count())
{
    if (dst_end_ > namespace_size_) {
        dst_end_ = namespace_size_;
    }
}

uint64_t LbaManager::get_and_advance_dst_lba(uint64_t total_blocks) {
    if (total_blocks == 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        return dst_current_;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Get current LBA
    uint64_t lba = dst_current_;
    
    // CRITICAL: Check if this operation would exceed the destination range
    // If so, wrap around to the start first
    if (lba + total_blocks > dst_end_) {
        // Operation would exceed the end - wrap to start
        lba = dst_start_;
        
        // CRITICAL: Check if the wrapped operation would still exceed bounds
        // If the operation itself is larger than the destination range, we can't fit it
        uint64_t range_size = dst_end_ - dst_start_;
        if (total_blocks > range_size) {
            // Operation is too large for the destination range - this is an error condition
            // For now, we'll still return the start LBA, but the caller should validate
            dst_current_ = dst_start_;
        } else {
            dst_current_ = dst_start_ + total_blocks;
        }
    } else {
        // Operation fits - advance normally
        dst_current_ += total_blocks;
    }
    
    // Check if the new position exceeds the end (for next operation)
    if (dst_current_ >= dst_end_) {
        // Wrap around to start for next operation
        dst_current_ = dst_start_;
    }
    
    return lba;
}

uint64_t LbaManager::get_next_dst_lba(uint64_t range_size) {
    // Just return the current LBA without advancing
    // The caller should call advance_dst_lba() after the operation completes
    // with the actual total_blocks that were copied
    (void)range_size; // Not used anymore, but kept for API compatibility
    
    std::lock_guard<std::mutex> lock(mutex_);
    return dst_current_;
}

void LbaManager::advance_dst_lba(uint64_t total_blocks) {
    if (total_blocks == 0) {
        return; // Nothing to advance
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Advance by total blocks
    dst_current_ += total_blocks;
    
    // Wrap around if we exceed the end
    // Check if the current position (after advance) would exceed the end for the next operation
    // We wrap when we've reached or exceeded the end
    if (dst_current_ >= dst_end_) {
        // Wrap around to start
        dst_current_ = dst_start_;
    }
}

uint64_t LbaManager::get_random_src_lba(uint64_t range_size) {
    if (range_size >= namespace_size_) {
        return 0;
    }
    
    // Limit source LBAs to first 90% of namespace to avoid edge cases
    // Some targets may have issues with reads near the end of the namespace
    uint64_t safe_namespace_size = (namespace_size_ * 90) / 100;
    if (safe_namespace_size < range_size) {
        safe_namespace_size = namespace_size_;  // Fallback if namespace is tiny
    }
    
    uint64_t max_lba = safe_namespace_size - range_size;
    std::uniform_int_distribution<uint64_t> dist(0, max_lba);
    
    return dist(rng_);
}

void LbaManager::reset_dst_lba() {
    std::lock_guard<std::mutex> lock(mutex_);
    dst_current_ = dst_start_;
}

void LbaManager::set_dst_range(uint64_t start, uint64_t end) {
    std::lock_guard<std::mutex> lock(mutex_);
    dst_start_ = start;
    dst_end_ = (end == 0 || end > namespace_size_) ? namespace_size_ : end;
    dst_current_ = dst_start_;
}

uint64_t LbaManager::get_dst_range_size() const {
    if (dst_end_ > dst_start_) {
        return dst_end_ - dst_start_;
    }
    return namespace_size_ - dst_start_;
}

} // namespace xload

