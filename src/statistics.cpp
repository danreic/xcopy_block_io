#include "statistics.h"
#include <cmath>
#include <algorithm>

namespace xload {

Statistics::Statistics()
    : operations_completed(0)
    , operations_failed(0)
    , bytes_copied(0)
    , total_latency_ns(0)
    , min_latency_ns(UINT64_MAX)
    , max_latency_ns(0)
    , max_samples(100000)
{
    latency_samples.reserve(max_samples);
}

// LOCK-FREE hot path - no mutex on the critical path
void Statistics::record_completion(uint64_t bytes, uint64_t latency_ns) {
    // All atomic operations - no locks
    operations_completed.fetch_add(1, std::memory_order_relaxed);
    bytes_copied.fetch_add(bytes, std::memory_order_relaxed);
    total_latency_ns.fetch_add(latency_ns, std::memory_order_relaxed);
    
    // Lock-free min update
    uint64_t current_min = min_latency_ns.load(std::memory_order_relaxed);
    while (latency_ns < current_min && 
           !min_latency_ns.compare_exchange_weak(current_min, latency_ns,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
        // current_min is updated by compare_exchange_weak
    }
    
    // Lock-free max update
    uint64_t current_max = max_latency_ns.load(std::memory_order_relaxed);
    while (latency_ns > current_max && 
           !max_latency_ns.compare_exchange_weak(current_max, latency_ns,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
        // current_max is updated by compare_exchange_weak
    }
    
    // Lock-free sample recording
    latency_buffer.record(latency_ns);
}

void Statistics::record_failure(uint16_t status_code) {
    operations_failed++;
    error_counter.record_error(status_code);
}

double Statistics::get_percentile(double p) const {
    // Get samples from lock-free buffer
    std::vector<uint64_t> sorted = latency_buffer.get_samples();
    
    if (sorted.empty()) {
        return 0.0;
    }
    
    std::sort(sorted.begin(), sorted.end());
    
    if (p <= 0.0) {
        return sorted[0];
    }
    if (p >= 100.0) {
        return sorted.back();
    }
    
    double index = (p / 100.0) * (sorted.size() - 1);
    size_t lower = static_cast<size_t>(index);
    size_t upper = lower + 1;
    
    if (upper >= sorted.size()) {
        return sorted[lower];
    }
    
    double fraction = index - lower;
    return sorted[lower] * (1.0 - fraction) + sorted[upper] * fraction;
}

double Statistics::get_std_dev() const {
    uint64_t completed = operations_completed.load(std::memory_order_relaxed);
    if (completed == 0) {
        return 0.0;
    }
    
    double avg = get_avg_latency_ns();
    double sum_sq_diff = 0.0;
    
    // Get samples from lock-free buffer
    std::vector<uint64_t> samples = latency_buffer.get_samples();
    for (uint64_t sample : samples) {
        double diff = static_cast<double>(sample) - avg;
        sum_sq_diff += diff * diff;
    }
    
    size_t sample_count = samples.size();
    if (sample_count == 0) {
        return 0.0;
    }
    
    return std::sqrt(sum_sq_diff / sample_count);
}

double Statistics::get_avg_latency_ns() const {
    uint64_t completed = operations_completed.load();
    if (completed == 0) {
        return 0.0;
    }
    return static_cast<double>(total_latency_ns.load()) / completed;
}

double Statistics::get_avg_latency_us() const {
    return get_avg_latency_ns() / 1000.0;
}

double Statistics::get_throughput_iops(double elapsed_sec) const {
    if (elapsed_sec <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(operations_completed.load()) / elapsed_sec;
}

double Statistics::get_throughput_mbps(double elapsed_sec) const {
    if (elapsed_sec <= 0.0) {
        return 0.0;
    }
    uint64_t bytes = bytes_copied.load();
    double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return mb / elapsed_sec;
}

void Statistics::reset() {
    operations_completed.store(0, std::memory_order_relaxed);
    operations_failed.store(0, std::memory_order_relaxed);
    bytes_copied.store(0, std::memory_order_relaxed);
    total_latency_ns.store(0, std::memory_order_relaxed);
    min_latency_ns.store(UINT64_MAX, std::memory_order_relaxed);
    max_latency_ns.store(0, std::memory_order_relaxed);
    
    latency_buffer.reset();
    latency_samples.clear();
    
    error_counter.reset();
}

void Statistics::get_snapshot(Statistics& snapshot) const {
    snapshot.operations_completed.store(operations_completed.load(std::memory_order_relaxed), std::memory_order_relaxed);
    snapshot.operations_failed.store(operations_failed.load(std::memory_order_relaxed), std::memory_order_relaxed);
    snapshot.bytes_copied.store(bytes_copied.load(std::memory_order_relaxed), std::memory_order_relaxed);
    snapshot.total_latency_ns.store(total_latency_ns.load(std::memory_order_relaxed), std::memory_order_relaxed);
    snapshot.min_latency_ns.store(min_latency_ns.load(std::memory_order_relaxed), std::memory_order_relaxed);
    snapshot.max_latency_ns.store(max_latency_ns.load(std::memory_order_relaxed), std::memory_order_relaxed);
    
    // Copy samples from lock-free buffer
    snapshot.latency_samples = latency_buffer.get_samples();
}

} // namespace xload

