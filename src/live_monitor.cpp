#include "live_monitor.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cmath>

namespace xload {

LiveMonitor::LiveMonitor(Statistics* stats, uint32_t interval_ms)
    : stats_(stats)
    , interval_ms_(interval_ms)
    , start_time_ns_(0)
    , last_update_ns_(0)
    , prev_ops_(0)
    , prev_bytes_(0)
    , prev_time_ns_(0)
    , use_ansi_(true)
    , first_line_(true)
{
}

void LiveMonitor::start() {
    start_time_ns_ = HighResTimer::now_ns();
    last_update_ns_ = start_time_ns_;
    prev_time_ns_ = start_time_ns_;
    prev_ops_ = 0;
    prev_bytes_ = 0;
    first_line_ = true;
}

void LiveMonitor::update() {
    if (interval_ms_ == 0) {
        return;  // Monitoring disabled
    }
    
    uint64_t now_ns = HighResTimer::now_ns();
    uint64_t elapsed_since_update_ns = now_ns - last_update_ns_;
    uint64_t interval_ns = static_cast<uint64_t>(interval_ms_) * 1000000ULL;
    
    if (elapsed_since_update_ns < interval_ns) {
        return;  // Not time to update yet
    }
    
    last_update_ns_ = now_ns;
    
    // Get current stats
    uint64_t curr_ops = stats_->operations_completed.load();
    uint64_t curr_bytes = stats_->bytes_copied.load();
    uint64_t failed = stats_->operations_failed.load();
    
    // Calculate instantaneous rates (since last update)
    double delta_sec = static_cast<double>(now_ns - prev_time_ns_) / 1e9;
    double inst_iops = 0.0;
    double inst_bw = 0.0;
    
    if (delta_sec > 0.0) {
        inst_iops = static_cast<double>(curr_ops - prev_ops_) / delta_sec;
        inst_bw = static_cast<double>(curr_bytes - prev_bytes_) / delta_sec;
    }
    
    // Calculate overall stats
    double total_elapsed_sec = static_cast<double>(now_ns - start_time_ns_) / 1e9;
    double avg_latency_us = stats_->get_avg_latency_us();
    
    // Save current values for next delta calculation
    prev_ops_ = curr_ops;
    prev_bytes_ = curr_bytes;
    prev_time_ns_ = now_ns;
    
    // Build status line similar to fio:
    // Jobs: 1 (f=1): [w(1)][100.0%][w=1234MiB/s][w=12.3k IOPS][eta 00m:00s]
    // We'll do: [00:00:05] BW=1234MiB/s | IOPS=12.3k | lat=1.23us | ops=12345 (err=0)
    
    std::ostringstream ss;
    
    ss << "[" << format_time(total_elapsed_sec) << "] ";
    ss << "BW=" << format_bytes(inst_bw) << "/s";
    ss << " | IOPS=" << format_iops(inst_iops);
    ss << " | lat=" << format_latency(avg_latency_us);
    ss << " | ops=" << format_count(curr_ops);
    if (failed > 0) {
        ss << " (\033[31merr=" << failed << "\033[0m)";
    }
    
    // Output the line
    if (use_ansi_) {
        // Use carriage return to overwrite the same line
        std::cout << "\r\033[K" << ss.str() << std::flush;
    } else {
        std::cout << ss.str() << std::endl;
    }
    
    first_line_ = false;
}

void LiveMonitor::finish() {
    if (interval_ms_ == 0) {
        return;
    }
    
    // Force one final update
    uint64_t now_ns = HighResTimer::now_ns();
    double total_elapsed_sec = static_cast<double>(now_ns - start_time_ns_) / 1e9;
    
    uint64_t total_ops = stats_->operations_completed.load();
    uint64_t total_bytes = stats_->bytes_copied.load();
    uint64_t failed = stats_->operations_failed.load();
    
    // Calculate average rates over entire run
    double avg_iops = total_elapsed_sec > 0 ? static_cast<double>(total_ops) / total_elapsed_sec : 0.0;
    double avg_bw = total_elapsed_sec > 0 ? static_cast<double>(total_bytes) / total_elapsed_sec : 0.0;
    double avg_latency_us = stats_->get_avg_latency_us();
    
    std::ostringstream ss;
    ss << "[" << format_time(total_elapsed_sec) << "] ";
    ss << "BW=" << format_bytes(avg_bw) << "/s";
    ss << " | IOPS=" << format_iops(avg_iops);
    ss << " | lat=" << format_latency(avg_latency_us);
    ss << " | ops=" << format_count(total_ops);
    if (failed > 0) {
        ss << " (err=" << failed << ")";
    }
    ss << " - DONE";
    
    if (use_ansi_) {
        std::cout << "\r\033[K" << ss.str() << std::endl;
    } else {
        std::cout << ss.str() << std::endl;
    }
}

std::string LiveMonitor::format_bytes(double bytes_per_sec) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1);
    
    if (bytes_per_sec >= 1e12) {
        ss << (bytes_per_sec / (1024.0 * 1024.0 * 1024.0 * 1024.0)) << "TiB";
    } else if (bytes_per_sec >= 1e9) {
        ss << (bytes_per_sec / (1024.0 * 1024.0 * 1024.0)) << "GiB";
    } else if (bytes_per_sec >= 1e6) {
        ss << (bytes_per_sec / (1024.0 * 1024.0)) << "MiB";
    } else if (bytes_per_sec >= 1e3) {
        ss << (bytes_per_sec / 1024.0) << "KiB";
    } else {
        ss << bytes_per_sec << "B";
    }
    
    return ss.str();
}

std::string LiveMonitor::format_iops(double iops) {
    std::ostringstream ss;
    ss << std::fixed;
    
    if (iops >= 1e6) {
        ss << std::setprecision(2) << (iops / 1e6) << "M";
    } else if (iops >= 1e3) {
        ss << std::setprecision(2) << (iops / 1e3) << "k";
    } else {
        ss << std::setprecision(0) << iops;
    }
    
    return ss.str();
}

std::string LiveMonitor::format_latency(double latency_us) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    
    if (latency_us >= 1000.0) {
        ss << (latency_us / 1000.0) << "ms";
    } else if (latency_us >= 1.0) {
        ss << latency_us << "us";
    } else {
        ss << (latency_us * 1000.0) << "ns";
    }
    
    return ss.str();
}

std::string LiveMonitor::format_time(double elapsed_sec) {
    int hours = static_cast<int>(elapsed_sec) / 3600;
    int mins = (static_cast<int>(elapsed_sec) % 3600) / 60;
    int secs = static_cast<int>(elapsed_sec) % 60;
    
    std::ostringstream ss;
    ss << std::setfill('0');
    
    if (hours > 0) {
        ss << std::setw(2) << hours << ":"
           << std::setw(2) << mins << ":"
           << std::setw(2) << secs;
    } else {
        ss << std::setw(2) << mins << ":"
           << std::setw(2) << secs;
    }
    
    return ss.str();
}

std::string LiveMonitor::format_count(uint64_t count) {
    std::ostringstream ss;
    
    if (count >= 1000000000ULL) {
        ss << std::fixed << std::setprecision(2) << (static_cast<double>(count) / 1e9) << "B";
    } else if (count >= 1000000ULL) {
        ss << std::fixed << std::setprecision(2) << (static_cast<double>(count) / 1e6) << "M";
    } else if (count >= 1000ULL) {
        ss << std::fixed << std::setprecision(2) << (static_cast<double>(count) / 1e3) << "k";
    } else {
        ss << count;
    }
    
    return ss.str();
}

} // namespace xload

