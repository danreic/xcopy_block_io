#ifndef STATISTICS_H
#define STATISTICS_H

#include <stdint.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <algorithm>
#include "error_handler.h"

namespace xload {

// Lock-free ring buffer for latency samples
class LockFreeLatencyBuffer {
public:
    static constexpr size_t BUFFER_SIZE = 65536; // Power of 2 for fast modulo
    
    LockFreeLatencyBuffer() : write_index_(0), sample_count_(0) {
        samples_ = new std::atomic<uint64_t>[BUFFER_SIZE];
        for (size_t i = 0; i < BUFFER_SIZE; i++) {
            samples_[i].store(0, std::memory_order_relaxed);
        }
    }
    
    ~LockFreeLatencyBuffer() {
        delete[] samples_;
    }
    
    // Delete copy/move constructors and assignment operators to prevent double-free
    LockFreeLatencyBuffer(const LockFreeLatencyBuffer&) = delete;
    LockFreeLatencyBuffer& operator=(const LockFreeLatencyBuffer&) = delete;
    LockFreeLatencyBuffer(LockFreeLatencyBuffer&&) = delete;
    LockFreeLatencyBuffer& operator=(LockFreeLatencyBuffer&&) = delete;
    
    // Lock-free sample recording
    void record(uint64_t latency_ns) {
        size_t idx = write_index_.fetch_add(1, std::memory_order_relaxed) & (BUFFER_SIZE - 1);
        samples_[idx].store(latency_ns, std::memory_order_relaxed);
        sample_count_.fetch_add(1, std::memory_order_relaxed);
    }
    
    // Get samples for percentile calculation (called from single thread during reporting)
    std::vector<uint64_t> get_samples() const {
        size_t count = std::min(sample_count_.load(std::memory_order_relaxed), BUFFER_SIZE);
        std::vector<uint64_t> result;
        result.reserve(count);
        for (size_t i = 0; i < count; i++) {
            uint64_t val = samples_[i].load(std::memory_order_relaxed);
            if (val > 0) {
                result.push_back(val);
            }
        }
        return result;
    }
    
    void reset() {
        write_index_.store(0, std::memory_order_relaxed);
        sample_count_.store(0, std::memory_order_relaxed);
        for (size_t i = 0; i < BUFFER_SIZE; i++) {
            samples_[i].store(0, std::memory_order_relaxed);
        }
    }
    
private:
    std::atomic<uint64_t>* samples_;
    std::atomic<size_t> write_index_;
    std::atomic<size_t> sample_count_;
};

struct Statistics {
    std::atomic<uint64_t> operations_completed;
    std::atomic<uint64_t> operations_failed;
    std::atomic<uint64_t> bytes_copied;
    std::atomic<uint64_t> total_latency_ns;
    std::atomic<uint64_t> min_latency_ns;
    std::atomic<uint64_t> max_latency_ns;
    
    // Lock-free latency buffer for percentile calculation
    LockFreeLatencyBuffer latency_buffer;
    
    // Legacy fields for compatibility
    std::vector<uint64_t> latency_samples;  // Used only during get_snapshot
    mutable std::mutex samples_mutex;       // Only used during snapshot/percentile calc
    size_t max_samples;
    
    // Error tracking
    ErrorHandler::ErrorCounter error_counter;
    
    Statistics();
    
    // Record completion (LOCK-FREE hot path)
    void record_completion(uint64_t bytes, uint64_t latency_ns);
    
    // Record failure
    void record_failure(uint16_t status_code);
    
    // Calculate percentiles
    double get_percentile(double p) const; // p in [0, 100]
    double get_p99() const { return get_percentile(99.0); }
    double get_p99_9() const { return get_percentile(99.9); }
    double get_p99_99() const { return get_percentile(99.99); }
    
    // Calculate standard deviation
    double get_std_dev() const;
    
    // Calculate average latency
    double get_avg_latency_ns() const;
    double get_avg_latency_us() const;
    
    // Calculate throughput
    double get_throughput_iops(double elapsed_sec) const;
    double get_throughput_mbps(double elapsed_sec) const;
    
    // Reset statistics
    void reset();
    
    // Get snapshot (thread-safe copy)
    void get_snapshot(Statistics& snapshot) const;
};

} // namespace xload

#endif // STATISTICS_H
