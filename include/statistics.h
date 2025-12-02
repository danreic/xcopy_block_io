#ifndef STATISTICS_H
#define STATISTICS_H

#include <stdint.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <algorithm>
#include "error_handler.h"

namespace xload {

struct Statistics {
    std::atomic<uint64_t> operations_completed;
    std::atomic<uint64_t> operations_failed;
    std::atomic<uint64_t> bytes_copied;
    std::atomic<uint64_t> total_latency_ns;
    std::atomic<uint64_t> min_latency_ns;
    std::atomic<uint64_t> max_latency_ns;
    
    // Latency samples for percentile calculation
    std::vector<uint64_t> latency_samples;
    mutable std::mutex samples_mutex;
    size_t max_samples;
    
    // Error tracking
    ErrorHandler::ErrorCounter error_counter;
    
    Statistics();
    
    // Record completion
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
