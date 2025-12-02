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

uint64_t LbaManager::get_next_dst_lba(uint64_t range_size) {
    if (dst_current_ + range_size > dst_end_) {
        // Wrap around to start
        dst_current_ = dst_start_;
    }
    
    uint64_t lba = dst_current_;
    dst_current_ += range_size;
    
    return lba;
}

uint64_t LbaManager::get_random_src_lba(uint64_t range_size) {
    if (range_size >= namespace_size_) {
        return 0;
    }
    
    uint64_t max_lba = namespace_size_ - range_size;
    std::uniform_int_distribution<uint64_t> dist(0, max_lba);
    
    return dist(rng_);
}

void LbaManager::reset_dst_lba() {
    dst_current_ = dst_start_;
}

void LbaManager::set_dst_range(uint64_t start, uint64_t end) {
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

