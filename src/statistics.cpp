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
    , max_samples(100000) // Keep up to 100k samples for percentile calculation
{
    latency_samples.reserve(max_samples);
}

void Statistics::record_completion(uint64_t bytes, uint64_t latency_ns) {
    operations_completed++;
    bytes_copied += bytes;
    total_latency_ns += latency_ns;
    
    uint64_t current_min = min_latency_ns.load();
    while (latency_ns < current_min && 
           !min_latency_ns.compare_exchange_weak(current_min, latency_ns)) {
        current_min = min_latency_ns.load();
    }
    
    uint64_t current_max = max_latency_ns.load();
    while (latency_ns > current_max && 
           !max_latency_ns.compare_exchange_weak(current_max, latency_ns)) {
        current_max = max_latency_ns.load();
    }
    
    // Store sample for percentile calculation
    {
        std::lock_guard<std::mutex> lock(samples_mutex);
        if (latency_samples.size() < max_samples) {
            latency_samples.push_back(latency_ns);
        } else {
            // Replace random sample to maintain distribution
            size_t idx = operations_completed.load() % max_samples;
            latency_samples[idx] = latency_ns;
        }
    }
}

void Statistics::record_failure(uint16_t status_code) {
    operations_failed++;
    error_counter.record_error(status_code);
}

double Statistics::get_percentile(double p) const {
    std::lock_guard<std::mutex> lock(samples_mutex);
    
    if (latency_samples.empty()) {
        return 0.0;
    }
    
    std::vector<uint64_t> sorted = latency_samples;
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
    uint64_t completed = operations_completed.load();
    if (completed == 0) {
        return 0.0;
    }
    
    double avg = get_avg_latency_ns();
    double sum_sq_diff = 0.0;
    
    {
        std::lock_guard<std::mutex> lock(samples_mutex);
        for (uint64_t sample : latency_samples) {
            double diff = static_cast<double>(sample) - avg;
            sum_sq_diff += diff * diff;
        }
    }
    
    return std::sqrt(sum_sq_diff / completed);
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
    operations_completed = 0;
    operations_failed = 0;
    bytes_copied = 0;
    total_latency_ns = 0;
    min_latency_ns = UINT64_MAX;
    max_latency_ns = 0;
    
    {
        std::lock_guard<std::mutex> lock(samples_mutex);
        latency_samples.clear();
    }
    
    error_counter.reset();
}

void Statistics::get_snapshot(Statistics& snapshot) const {
    snapshot.operations_completed = operations_completed.load();
    snapshot.operations_failed = operations_failed.load();
    snapshot.bytes_copied = bytes_copied.load();
    snapshot.total_latency_ns = total_latency_ns.load();
    snapshot.min_latency_ns = min_latency_ns.load();
    snapshot.max_latency_ns = max_latency_ns.load();
    
    {
        std::lock_guard<std::mutex> lock(samples_mutex);
        snapshot.latency_samples = latency_samples;
    }
}

} // namespace xload

