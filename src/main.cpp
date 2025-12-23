#include <iostream>
#include <csignal>
#include <vector>
#include <memory>
#include <unistd.h>
#include "config_manager.h"
#include "spdk_context.h"
#include "poll_thread_manager.h"
#include "xcopy_generator.h"
#include "lba_manager.h"
#include "statistics.h"
#include "json_reporter.h"
#include "high_res_timer.h"
#include "live_monitor.h"

namespace xload {

static volatile bool g_running = true;

void signal_handler(int signum) {
    (void)signum;
    g_running = false;
}

} // namespace xload

int main(int argc, char** argv) {
    using namespace xload;
    
    // Parse configuration
    Config config;
    if (ConfigManager::parse_args(argc, argv, config) != 0) {
        return 1;
    }
    
    // Set verbose flag for all modules
    SpdkContext::set_verbose(config.verbose);
    
    if (config.verbose) {
        config.print();
    }
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize SPDK context (multi-path: connect to all provided addresses)
    SpdkContext spdk_ctx;
    if (spdk_ctx.init(config.traddrs, config.trsvcid, 
                      config.hostnqn, config.subnqn) != 0) {
        std::cerr << "Failed to initialize SPDK context" << std::endl;
        return 1;
    }
    
    if (config.verbose) {
        std::cout << "Connected to " << spdk_ctx.get_ctrlr_count() << " NVMe controller(s)" << std::endl;
        std::cout << "Namespaces:" << std::endl;
        for (const auto& ns : spdk_ctx.get_namespaces()) {
            std::cout << "  NSID " << ns.nsid << ": " 
                      << ns.size_blocks << " blocks, "
                      << ns.block_size << " bytes/block" << std::endl;
        }
    }
    
    // Get destination namespace info
    const NamespaceInfo* dst_ns = spdk_ctx.get_ns_info(config.dst_nsid);
    if (!dst_ns) {
        std::cerr << "Error: Destination namespace " << config.dst_nsid 
                  << " not found" << std::endl;
        spdk_ctx.cleanup();
        return 1;
    }
    
    // Check cross-namespace copy support if enabled
    if (config.enable_cross_namespace) {
        if (!spdk_ctx.supports_cross_namespace_copy()) {
            std::cerr << "Warning: Cross-namespace copy is enabled but target may not support TP4130" << std::endl;
        }
    }
    
    // Prepare namespace list for generator
    std::vector<const struct spdk_nvme_ns*> namespaces;
    for (const auto& ns_info : spdk_ctx.get_namespaces()) {
        namespaces.push_back(ns_info.ns);
    }
    
    // Initialize LBA manager
    uint64_t dst_lba_end = config.dst_lba_end;
    if (dst_lba_end == 0) {
        dst_lba_end = dst_ns->size_blocks;
    }
    LbaManager lba_mgr(dst_ns->size_blocks, config.dst_lba_start, dst_lba_end);
    
    // Initialize XCOPY generator
    bool target_supports_cross_ns = spdk_ctx.supports_cross_namespace_copy();
    XcopyGenerator generator(config.max_ranges, config.src_nsids, 
                             config.dst_nsid, namespaces,
                             config.enable_cross_namespace,
                             target_supports_cross_ns);
    
    // Warn if cross-namespace is enabled but target doesn't support it
    if (config.enable_cross_namespace && !target_supports_cross_ns) {
        std::cerr << "Warning: Cross-namespace copy is enabled but target may not support TP4130" << std::endl;
        std::cerr << "         Commands will use format 0 (same-namespace) as fallback" << std::endl;
    }
    
    // Initialize statistics
    Statistics stats;
    
    // Use range size from configuration (default: 2048 blocks = 1MB at 512B/block)
    uint64_t range_size = config.range_size;
    
    // Initialize poll thread manager
    PollThreadManager thread_mgr(&spdk_ctx, config.num_cores, config.iodepth,
                                 &generator, &lba_mgr, range_size, &stats);
    
    // Start threads
    if (thread_mgr.start() != 0) {
        std::cerr << "Failed to start poll threads" << std::endl;
        spdk_ctx.cleanup();
        return 1;
    }
    
    if (config.verbose) {
        std::cout << "Started " << config.num_cores << " poll threads" << std::endl;
        std::cout << "Running workload..." << std::endl;
    }
    
    // Initialize live monitor for real-time BW/IOPS display
    // Disable live monitor if JSON output is enabled (to avoid mixing outputs)
    LiveMonitor monitor(&stats, config.json_output ? 0 : config.status_interval_ms);
    
    // Run for specified duration
    uint64_t start_time_ns = HighResTimer::now_ns();
    uint64_t runtime_ns = config.runtime_sec * 1000000000ULL;
    
    monitor.start();
    g_running = true;
    while (g_running) {
        if (config.runtime_sec > 0) {
            uint64_t elapsed_ns = HighResTimer::now_ns() - start_time_ns;
            if (elapsed_ns >= runtime_ns) {
                break;
            }
        }
        
        // Update live monitor display
        monitor.update();
        
        // Sleep briefly to avoid busy-waiting
        // Use smaller sleep when monitor is active for smoother updates
        usleep(monitor.is_enabled() ? 50000 : 100000); // 50ms or 100ms
    }
    
    // Finalize live monitor display
    monitor.finish();
    
    // Stop threads
    thread_mgr.stop();
    thread_mgr.wait();
    
    // Calculate elapsed time
    uint64_t end_time_ns = HighResTimer::now_ns();
    double elapsed_sec = HighResTimer::ns_to_ms(end_time_ns - start_time_ns) / 1000.0;
    
    // Generate report
    if (config.json_output) {
        JsonReporter::generate_report(stats, config, elapsed_sec, std::cout);
    } else {
        JsonReporter::generate_human_report(stats, config, elapsed_sec, std::cout);
    }
    
    // Cleanup
    spdk_ctx.cleanup();
    
    return 0;
}

