#ifndef XCOPY_TOOL_H
#define XCOPY_TOOL_H

#include <stdint.h>
#include <stdbool.h>

// Maximum number of ranges per NVMe Copy command
#define MAX_COPY_RANGES 16

// Application configuration
struct xcopy_config {
    // Transport configuration (can be auto-detected from device paths)
    char *transport_type;      // "tcp", "pcie", "rdma"
    char *traddr;              // Transport address (IP for TCP, PCIe address for PCIe)
    char *trsvcid;             // Transport service ID (port for TCP)
    char *subnqn;              // Subsystem NQN
    
    // Device paths (alternative to manual transport/nsid specification)
    char *src_device;          // Source device path (e.g., /dev/nvme1n1)
    char *dst_device;          // Destination device path (e.g., /dev/nvme1n2)
    
    // Workload configuration
    uint32_t num_ranges;       // Number of concurrent ranges
    uint64_t range_size;       // Size of each range in blocks
    uint64_t src_lba_start;    // Starting LBA for source
    uint64_t dst_lba_start;    // Starting LBA for destination
    uint32_t src_nsid;         // Source namespace ID (auto-detected from src_device if provided)
    uint32_t dst_nsid;         // Destination namespace ID (auto-detected from dst_device if provided)
    
    // Concurrency configuration
    uint32_t num_threads;      // Number of worker threads (0 = auto-detect)
    uint32_t queue_depth;      // Queue depth per thread
    
    // Runtime configuration
    uint64_t duration_sec;     // Test duration in seconds (0 = infinite)
    uint64_t num_operations;   // Number of operations to perform (0 = infinite)
    bool verbose;              // Verbose output
};

// Statistics
struct xcopy_stats {
    uint64_t operations_completed;
    uint64_t operations_failed;
    uint64_t bytes_copied;
    uint64_t total_latency_us; // Total latency in microseconds
    uint64_t min_latency_us;
    uint64_t max_latency_us;
    double throughput_mbps;    // Throughput in MB/s
};

// Initialize the tool
int xcopy_tool_init(struct xcopy_config *config);

// Run the workload
int xcopy_tool_run(struct xcopy_config *config);

// Cleanup
void xcopy_tool_cleanup(void);

// Get statistics
void xcopy_tool_get_stats(struct xcopy_stats *stats);

// Print statistics
void xcopy_tool_print_stats(struct xcopy_stats *stats);

#endif // XCOPY_TOOL_H

