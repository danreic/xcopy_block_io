#ifndef LIVE_MONITOR_H
#define LIVE_MONITOR_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include "statistics.h"
#include "high_res_timer.h"

namespace xload {

/**
 * LiveMonitor - Real-time BW/IOPS display similar to fio's status line
 * 
 * Displays a continuously updating status line showing:
 * - Elapsed time
 * - Current BW (MB/s and MiB/s)
 * - Current IOPS
 * - Average latency
 * - Operations completed
 */
class LiveMonitor {
public:
    /**
     * Constructor
     * @param stats Pointer to shared Statistics object
     * @param interval_ms Status update interval in milliseconds (default: 1000ms)
     */
    LiveMonitor(Statistics* stats, uint32_t interval_ms = 1000);
    
    /**
     * Start monitoring - call once before the main loop
     */
    void start();
    
    /**
     * Update display if interval has elapsed
     * Call this from the main loop - it will only update at the configured interval
     */
    void update();
    
    /**
     * Force a final update and move to next line
     * Call this when the run completes
     */
    void finish();
    
    /**
     * Check if monitoring is enabled
     */
    bool is_enabled() const { return interval_ms_ > 0; }
    
    /**
     * Set whether to use ANSI escape codes for in-place updates
     * @param use_ansi true to update line in-place (default), false for new lines
     */
    void set_use_ansi(bool use_ansi) { use_ansi_ = use_ansi; }

private:
    Statistics* stats_;
    uint32_t interval_ms_;
    
    // Timing
    uint64_t start_time_ns_;
    uint64_t last_update_ns_;
    
    // Previous values for calculating deltas (instantaneous rates)
    uint64_t prev_ops_;
    uint64_t prev_bytes_;
    uint64_t prev_time_ns_;
    
    // Display options
    bool use_ansi_;
    bool first_line_;
    
    // Format helpers
    static std::string format_bytes(double bytes_per_sec);
    static std::string format_iops(double iops);
    static std::string format_latency(double latency_us);
    static std::string format_time(double elapsed_sec);
    static std::string format_count(uint64_t count);
};

} // namespace xload

#endif // LIVE_MONITOR_H

