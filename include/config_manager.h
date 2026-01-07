#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <string>
#include <vector>

namespace xload {

struct Config {
    // Transport configuration
    std::vector<std::string> traddrs;  // Target IP addresses (multiple for multi-path)
    std::string trsvcid;               // Service ID (port)
    std::string hostnqn;               // Host NQN
    std::string subnqn;                // Subsystem NQN (optional, for discovery)
    
    // Runtime configuration
    uint64_t runtime_sec;        // Runtime duration in seconds (0 = infinite)
    uint32_t iodepth;            // Maximum global I/O depth
    uint32_t num_cores;          // Number of dedicated CPU cores
    uint32_t max_ranges;         // Maximum source ranges per command (1-16)
    uint32_t fixed_ranges;       // Fixed number of ranges per command (0 = random, 1-16 = fixed)
    
    // Workload configuration
    uint32_t dst_nsid;           // Destination namespace ID
    std::vector<uint32_t> src_nsids;  // Source namespace IDs (for cross-namespace)
    uint64_t dst_lba_start;      // Starting LBA for destination
    uint64_t dst_lba_end;        // Ending LBA for destination (0 = use namespace size)
    uint64_t range_size;         // Size of each range in blocks (default: 2048 = 1MB at 512B/block)
    bool enable_cross_namespace; // Enable cross-namespace copy (format 2, requires TP4130)
    
    // Output configuration
    bool json_output;            // Output in JSON format
    bool verbose;                // Verbose output
    uint32_t status_interval_ms; // Live status update interval in ms (0 = disabled)
    
    // Default constructor
    Config();
    
    // Validate configuration
    bool validate() const;
    
    // Print configuration
    void print() const;
};

class ConfigManager {
public:
    // Parse command line arguments
    static int parse_args(int argc, char** argv, Config& config);
    
    // Print usage
    static void print_usage(const char* prog_name);
};

} // namespace xload

#endif // CONFIG_MANAGER_H

