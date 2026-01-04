#include "json_reporter.h"
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace xload {

void JsonReporter::generate_report(const Statistics& stats,
                                   const Config& config,
                                   double elapsed_sec,
                                   ::std::ostream& out) {
    Statistics snapshot;
    stats.get_snapshot(snapshot);
    
    out << "{\n";
    out << "  \"configuration\": {\n";
    out << "    \"traddrs\": [";
    for (size_t i = 0; i < config.traddrs.size(); ++i) {
        out << "\"" << config.traddrs[i] << "\"";
        if (i < config.traddrs.size() - 1) out << ", ";
    }
    out << "],\n";
    out << "    \"trsvcid\": \"" << config.trsvcid << "\",\n";
    out << "    \"hostnqn\": \"" << config.hostnqn << "\",\n";
    out << "    \"runtime_sec\": " << config.runtime_sec << ",\n";
    out << "    \"iodepth\": " << config.iodepth << ",\n";
    out << "    \"num_cores\": " << config.num_cores << ",\n";
    out << "    \"max_ranges\": " << config.max_ranges << ",\n";
    out << "    \"dst_nsid\": " << config.dst_nsid << "\n";
    out << "  },\n";
    
    out << "  \"runtime\": {\n";
    out << "    \"elapsed_sec\": " << std::fixed << std::setprecision(3) << elapsed_sec << "\n";
    out << "  },\n";
    
    out << "  \"statistics\": {\n";
    out << "    \"operations_completed\": " << snapshot.operations_completed.load() << ",\n";
    out << "    \"operations_failed\": " << snapshot.operations_failed.load() << ",\n";
    out << "    \"bytes_copied\": " << snapshot.bytes_copied.load() << ",\n";
    out << "    \"throughput\": {\n";
    out << "      \"iops\": " << std::fixed << std::setprecision(2) 
        << snapshot.get_throughput_iops(elapsed_sec) << ",\n";
    out << "      \"mbps\": " << std::fixed << std::setprecision(2) 
        << snapshot.get_throughput_mbps(elapsed_sec) << "\n";
    out << "    },\n";
    
    out << "    \"latency\": {\n";
    out << "      \"avg_us\": " << std::fixed << std::setprecision(2) 
        << snapshot.get_avg_latency_us() << ",\n";
    out << "      \"min_us\": " << std::fixed << std::setprecision(2) 
        << (snapshot.min_latency_ns.load() / 1000.0) << ",\n";
    out << "      \"max_us\": " << std::fixed << std::setprecision(2) 
        << (snapshot.max_latency_ns.load() / 1000.0) << ",\n";
    out << "      \"std_dev_us\": " << std::fixed << std::setprecision(2) 
        << (snapshot.get_std_dev() / 1000.0) << ",\n";
    out << "      \"p99_us\": " << std::fixed << std::setprecision(2) 
        << (snapshot.get_p99() / 1000.0) << ",\n";
    out << "      \"p99_9_us\": " << std::fixed << std::setprecision(2) 
        << (snapshot.get_p99_9() / 1000.0) << ",\n";
    out << "      \"p99_99_us\": " << std::fixed << std::setprecision(2) 
        << (snapshot.get_p99_99() / 1000.0) << "\n";
    out << "    },\n";
    
    // Error statistics
    out << "    \"errors\": {\n";
    out << "      \"total\": " << snapshot.error_counter.get_total_errors() << ",\n";
    out << "      \"saturation\": " << snapshot.error_counter.get_count(0x0807) 
        << ",\n";
    out << "      \"insufficient_resources\": " << snapshot.error_counter.get_count(0x0189) 
        << "\n";
    out << "    }\n";
    
    out << "  }\n";
    out << "}\n";
}

void JsonReporter::generate_human_report(const Statistics& stats,
                                        const Config& /* config */,
                                        double elapsed_sec,
                                        ::std::ostream& out) {
    Statistics snapshot;
    stats.get_snapshot(snapshot);
    
    out << "\n=== X-LOAD Workload Complete ===\n";
    out << "Duration: " << std::fixed << std::setprecision(2) << elapsed_sec << " seconds\n";
    out << "Operations completed: " << snapshot.operations_completed.load() << "\n";
    out << "Operations failed: " << snapshot.operations_failed.load() << "\n";
    out << "Bytes copied: " << snapshot.bytes_copied.load() 
        << " (" << std::fixed << std::setprecision(2) 
        << (snapshot.bytes_copied.load() / (1024.0 * 1024.0)) << " MB)\n";
    
    out << "\nThroughput:\n";
    out << "  IOPs: " << std::fixed << std::setprecision(2) 
        << snapshot.get_throughput_iops(elapsed_sec) << "\n";
    out << "  MB/s: " << std::fixed << std::setprecision(2) 
        << snapshot.get_throughput_mbps(elapsed_sec) << "\n";
    
    out << "\nLatency (microseconds):\n";
    out << "  Average: " << std::fixed << std::setprecision(2) 
        << snapshot.get_avg_latency_us() << "\n";
    out << "  Min: " << std::fixed << std::setprecision(2) 
        << (snapshot.min_latency_ns.load() / 1000.0) << "\n";
    out << "  Max: " << std::fixed << std::setprecision(2) 
        << (snapshot.max_latency_ns.load() / 1000.0) << "\n";
    out << "  Std Dev: " << std::fixed << std::setprecision(2) 
        << (snapshot.get_std_dev() / 1000.0) << "\n";
    out << "  P99: " << std::fixed << std::setprecision(2) 
        << (snapshot.get_p99() / 1000.0) << "\n";
    out << "  P99.9: " << std::fixed << std::setprecision(2) 
        << (snapshot.get_p99_9() / 1000.0) << "\n";
    out << "  P99.99: " << std::fixed << std::setprecision(2) 
        << (snapshot.get_p99_99() / 1000.0) << "\n";
    
    uint64_t total_errors = snapshot.error_counter.get_total_errors();
    if (total_errors > 0) {
        out << "\nErrors:\n";
        out << "  Total: " << total_errors << "\n";
        uint64_t queue_full = snapshot.error_counter.get_count(0x0807);
        uint64_t insufficient = snapshot.error_counter.get_count(0x0189);
        if (queue_full > 0) {
            out << "  Queue Full (0x807): " << queue_full << "\n";
        }
        if (insufficient > 0) {
            out << "  Insufficient Resources (0x189): " << insufficient << "\n";
        }
    }
    
    out << "==================================\n";
}

} // namespace xload

