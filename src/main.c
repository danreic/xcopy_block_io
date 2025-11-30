#include "xcopy_tool.h"
#include "nvme_wrapper.h"
#include "xcopy_cmd.h"
#include "range_manager.h"
#include "volume_manager.h"
#include "concurrency_manager.h"
#include "statistics.h"
#include "device_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdbool.h>

// Global context for signal handling
static volatile bool g_running = true;

// Signal handler
static void signal_handler(int signum) {
    (void)signum;
    g_running = false;
}

// Print usage
static void print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS]\n", prog_name);
    printf("\n");
    printf("Transport Options:\n");
    printf("  -t, --transport TYPE      Transport type: tcp, pcie, rdma (default: tcp)\n");
    printf("  -a, --traddr ADDR         Transport address (IP for TCP, PCIe address for PCIe)\n");
    printf("  -s, --trsvcid PORT        Transport service ID (port for TCP, default: 4420)\n");
    printf("  -n, --subnqn NQN          Subsystem NQN (required for TCP/RDMA)\n");
    printf("\n");
    printf("Workload Options:\n");
    printf("  --src-device PATH         Source device path (e.g., /dev/nvme1n1)\n");
    printf("  --dst-device PATH         Destination device path (e.g., /dev/nvme1n2)\n");
    printf("  --src-nsid NSID           Source namespace ID (default: 1, auto-detected from --src-device)\n");
    printf("  --dst-nsid NSID           Destination namespace ID (default: 1, auto-detected from --dst-device)\n");
    printf("  --src-lba LBA             Starting LBA for source (default: 0)\n");
    printf("  --dst-lba LBA             Starting LBA for destination (default: 0)\n");
    printf("  --range-size SIZE         Size of each range in blocks (default: 8)\n");
    printf("  --num-ranges NUM          Number of concurrent ranges (default: 1, max: 16)\n");
    printf("\n");
    printf("Concurrency Options:\n");
    printf("  --threads NUM             Number of worker threads (default: 1)\n");
    printf("  --queue-depth DEPTH       Queue depth per thread (default: 64)\n");
    printf("\n");
    printf("Runtime Options:\n");
    printf("  -d, --duration SEC        Test duration in seconds (0 = infinite, default: 10)\n");
    printf("  -c, --count NUM           Number of operations to perform (0 = infinite)\n");
    printf("  -v, --verbose             Verbose output\n");
    printf("  -h, --help                Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  # Using device paths (recommended - auto-detects transport and namespace IDs)\n");
    printf("  %s --src-device /dev/nvme1n1 --dst-device /dev/nvme1n2\n", prog_name);
    printf("\n");
    printf("  # NVMe-TCP with manual settings\n");
    printf("  %s -t tcp -a 192.168.1.100 -n nqn.2016-06.io.spdk:cnode1 --src-nsid 1 --dst-nsid 2\n", prog_name);
    printf("\n");
    printf("  # PCIe device\n");
    printf("  %s -t pcie -a 0000:01:00.0 --src-nsid 1 --dst-nsid 2\n", prog_name);
    printf("\n");
}

// Parse command line arguments
static int parse_args(int argc, char **argv, struct xcopy_config *config) {
    int opt;
    int option_index = 0;
    
    static struct option long_options[] = {
        {"transport", required_argument, 0, 't'},
        {"traddr", required_argument, 0, 'a'},
        {"trsvcid", required_argument, 0, 's'},
        {"subnqn", required_argument, 0, 'n'},
        {"src-device", required_argument, 0, 1008},
        {"dst-device", required_argument, 0, 1009},
        {"src-nsid", required_argument, 0, 1000},
        {"dst-nsid", required_argument, 0, 1001},
        {"src-lba", required_argument, 0, 1002},
        {"dst-lba", required_argument, 0, 1003},
        {"range-size", required_argument, 0, 1004},
        {"num-ranges", required_argument, 0, 1005},
        {"threads", required_argument, 0, 1006},
        {"queue-depth", required_argument, 0, 1007},
        {"duration", required_argument, 0, 'd'},
        {"count", required_argument, 0, 'c'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    // Set defaults
    // Default to TCP (will be overridden if device paths are provided and transport is detected)
    config->transport_type = "tcp";
    config->traddr = NULL;
    config->trsvcid = "4420";
    config->subnqn = NULL;
    config->src_device = NULL;
    config->dst_device = NULL;
    config->src_nsid = 1;
    config->dst_nsid = 1;
    config->src_lba_start = 0;
    config->dst_lba_start = 0;
    config->range_size = 8;
    config->num_ranges = 1;
    config->num_threads = 1;
    config->queue_depth = 64;
    config->duration_sec = 10;
    config->num_operations = 0;
    config->verbose = false;
    
    while ((opt = getopt_long(argc, argv, "t:a:s:n:d:c:vh", long_options, &option_index)) != -1) {
        switch (opt) {
            case 't':
                config->transport_type = optarg;
                break;
            case 'a':
                config->traddr = optarg;
                break;
            case 's':
                config->trsvcid = optarg;
                break;
            case 'n':
                config->subnqn = optarg;
                break;
            case 'd':
                config->duration_sec = strtoul(optarg, NULL, 0);
                break;
            case 'c':
                config->num_operations = strtoull(optarg, NULL, 0);
                break;
            case 'v':
                config->verbose = true;
                break;
            case 'h':
                print_usage(argv[0]);
                return 1;
            case 1000:
                config->src_nsid = strtoul(optarg, NULL, 0);
                break;
            case 1001:
                config->dst_nsid = strtoul(optarg, NULL, 0);
                break;
            case 1002:
                config->src_lba_start = strtoull(optarg, NULL, 0);
                break;
            case 1003:
                config->dst_lba_start = strtoull(optarg, NULL, 0);
                break;
            case 1004:
                config->range_size = strtoull(optarg, NULL, 0);
                break;
            case 1005:
                config->num_ranges = strtoul(optarg, NULL, 0);
                if (config->num_ranges > MAX_COPY_RANGES) {
                    fprintf(stderr, "Error: num-ranges cannot exceed %d\n", MAX_COPY_RANGES);
                    return -1;
                }
                break;
            case 1006:
                config->num_threads = strtoul(optarg, NULL, 0);
                break;
            case 1007:
                config->queue_depth = strtoul(optarg, NULL, 0);
                break;
            case 1008:
                config->src_device = optarg;
                break;
            case 1009:
                config->dst_device = optarg;
                break;
            default:
                print_usage(argv[0]);
                return -1;
        }
    }
    
    // If device paths are provided, parse them and auto-detect transport/nsid
    // Use static buffers for auto-detected transport info
    static char auto_transport_type[16];
    static char auto_traddr[256];
    static char auto_trsvcid[16];
    static char auto_subnqn[256];
    
    if (config->src_device || config->dst_device) {
        struct device_info src_info, dst_info;
        
        if (config->src_device) {
            if (!device_parser_device_exists(config->src_device)) {
                fprintf(stderr, "Error: Source device %s does not exist\n", config->src_device);
                return -1;
            }
            if (device_parser_read_transport(config->src_device, &src_info) != 0) {
                fprintf(stderr, "Error: Failed to parse source device %s\n", config->src_device);
                return -1;
            }
            config->src_nsid = src_info.namespace_id;
            
            // Use transport info from source device if not manually specified
            // Always use detected transport type when device path is provided
            if (strlen(src_info.transport_type) > 0) {
                strncpy(auto_transport_type, src_info.transport_type, sizeof(auto_transport_type) - 1);
                auto_transport_type[sizeof(auto_transport_type) - 1] = '\0';
                // Normalize transport type (handle case variations)
                if (strcasecmp(auto_transport_type, "PCIe") == 0) {
                    strcpy(auto_transport_type, "pcie");
                }
                config->transport_type = auto_transport_type;
            } else {
                // If transport detection failed, default to PCIe for local devices
                strcpy(auto_transport_type, "pcie");
                config->transport_type = auto_transport_type;
            }
            if (!config->traddr && strlen(src_info.traddr) > 0) {
                strncpy(auto_traddr, src_info.traddr, sizeof(auto_traddr) - 1);
                auto_traddr[sizeof(auto_traddr) - 1] = '\0';
                config->traddr = auto_traddr;
            }
            if (!config->trsvcid && strlen(src_info.trsvcid) > 0) {
                strncpy(auto_trsvcid, src_info.trsvcid, sizeof(auto_trsvcid) - 1);
                auto_trsvcid[sizeof(auto_trsvcid) - 1] = '\0';
                config->trsvcid = auto_trsvcid;
            }
            if (!config->subnqn && strlen(src_info.subnqn) > 0) {
                strncpy(auto_subnqn, src_info.subnqn, sizeof(auto_subnqn) - 1);
                auto_subnqn[sizeof(auto_subnqn) - 1] = '\0';
                config->subnqn = auto_subnqn;
            }
            
            // Debug output if verbose and subnqn is missing
            if (config->verbose && !config->subnqn && 
                (strcmp(config->transport_type, "tcp") == 0 || strcmp(config->transport_type, "rdma") == 0)) {
                fprintf(stderr, "Warning: Subsystem NQN not found in sysfs for device %s\n", config->src_device);
                fprintf(stderr, "  Tried reading from: /sys/class/nvme/nvme%u/address and /sys/class/nvme/nvme%u/subsysnqn\n",
                        src_info.controller_id, src_info.controller_id);
            }
        }
        
        if (config->dst_device) {
            if (!device_parser_device_exists(config->dst_device)) {
                fprintf(stderr, "Error: Destination device %s does not exist\n", config->dst_device);
                return -1;
            }
            if (device_parser_read_transport(config->dst_device, &dst_info) != 0) {
                fprintf(stderr, "Error: Failed to parse destination device %s\n", config->dst_device);
                return -1;
            }
            config->dst_nsid = dst_info.namespace_id;
            
            // Verify both devices are on the same controller/transport
            if (config->src_device && config->dst_device) {
                if (src_info.controller_id != dst_info.controller_id) {
                    fprintf(stderr, "Warning: Source and destination devices are on different controllers\n");
                    fprintf(stderr, "  Source: controller %u, Destination: controller %u\n",
                            src_info.controller_id, dst_info.controller_id);
                    fprintf(stderr, "  Cross-controller XCOPY may not be supported\n");
                }
            }
        }
    }
    
    // Validate required options for TCP/RDMA (if not using device paths)
    if (!config->src_device && !config->dst_device) {
        if (strcmp(config->transport_type, "tcp") == 0 || strcmp(config->transport_type, "rdma") == 0) {
            if (!config->traddr || !config->subnqn) {
                fprintf(stderr, "Error: traddr and subnqn are required for TCP/RDMA transport\n");
                fprintf(stderr, "  Or use --src-device and --dst-device to auto-detect\n");
                return -1;
            }
        }
    }
    
    return 0;
}

int main(int argc, char **argv) {
    struct xcopy_config config;
    struct xcopy_transport_config transport;
    struct nvme_context nvme_ctx;
    struct volume_manager vol_mgr;
    struct concurrency_manager concurrency_mgr;
    struct range_generator range_gen;
    struct xcopy_stats stats;
    
    int rc = 0;
    
    // Parse command line arguments
    if (parse_args(argc, argv, &config) != 0) {
        return 1;
    }
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize transport configuration
    transport.type = nvme_wrapper_parse_transport(config.transport_type);
    transport.traddr = config.traddr;
    transport.trsvcid = config.trsvcid;
    transport.subnqn = config.subnqn;
    transport.hostnqn = NULL;
    
    // Initialize NVMe
    if (config.verbose) {
        printf("Initializing NVMe...\n");
        printf("Transport type: %s\n", config.transport_type);
        if (config.traddr) {
            printf("Transport address: %s\n", config.traddr);
        }
        if (config.trsvcid) {
            printf("Transport service ID: %s\n", config.trsvcid);
        }
        if (config.subnqn) {
            printf("Subsystem NQN: %s\n", config.subnqn);
        }
    }
    
    if (nvme_wrapper_init(&nvme_ctx, &transport) != 0) {
        fprintf(stderr, "Failed to initialize NVMe\n");
        return 1;
    }
    
    if (nvme_wrapper_connect(&nvme_ctx, &transport) != 0) {
        fprintf(stderr, "Failed to connect to NVMe controller\n");
        nvme_wrapper_cleanup(&nvme_ctx);
        return 1;
    }
    
    struct nvme_ctrl *ctrl = nvme_wrapper_get_ctrl(&nvme_ctx);
    if (!ctrl) {
        fprintf(stderr, "No controller found\n");
        nvme_wrapper_cleanup(&nvme_ctx);
        return 1;
    }
    
    if (config.verbose) {
        printf("Connected to NVMe controller\n");
    }
    
    // Initialize volume manager
    if (volume_manager_init(&vol_mgr) != 0) {
        fprintf(stderr, "Failed to initialize volume manager\n");
        nvme_wrapper_cleanup(&nvme_ctx);
        return 1;
    }
    
    // Discover and add namespaces
    // libnvme uses nsid starting from 1, and we iterate through all possible nsids
    // The actual number of namespaces is determined by the controller
    for (uint32_t nsid = 1; nsid <= 256; nsid++) {
        struct nvme_ns *ns = nvme_wrapper_get_ns(&nvme_ctx, nsid);
        if (ns) {
            // Check if namespace is active by checking if it has valid size
            uint64_t ns_size = nvme_wrapper_get_ns_size(&nvme_ctx, nsid);
            if (ns_size > 0) {
                volume_manager_add(&vol_mgr, nsid, ns, ctrl);
            }
        }
    }
    
    if (config.verbose) {
        volume_manager_print_volumes(&vol_mgr);
    }
    
    // Validate namespaces
    struct volume_info *src_vol = volume_manager_get(&vol_mgr, config.src_nsid);
    struct volume_info *dst_vol = volume_manager_get(&vol_mgr, config.dst_nsid);
    
    if (!src_vol || !dst_vol) {
        fprintf(stderr, "Error: Invalid namespace ID (src=%u, dst=%u)\n",
                config.src_nsid, config.dst_nsid);
        volume_manager_cleanup(&vol_mgr);
        nvme_wrapper_cleanup(&nvme_ctx);
        return 1;
    }
    
    if (!volume_manager_validate_cross_volume(&vol_mgr, config.src_nsid, config.dst_nsid)) {
        fprintf(stderr, "Error: Namespaces are not compatible for cross-volume copy\n");
        volume_manager_cleanup(&vol_mgr);
        nvme_wrapper_cleanup(&nvme_ctx);
        return 1;
    }
    
    // Initialize range generator
    struct range_workload workload;
    workload.src_nsid = config.src_nsid;
    workload.dst_nsid = config.dst_nsid;
    workload.src_lba_start = config.src_lba_start;
    workload.dst_lba_start = config.dst_lba_start;
    workload.range_size = config.range_size;
    workload.num_ranges = config.num_ranges;
    workload.namespace_size = src_vol->size_blocks;
    
    if (range_generator_init(&range_gen, &workload) != 0) {
        fprintf(stderr, "Failed to initialize range generator\n");
        volume_manager_cleanup(&vol_mgr);
        nvme_wrapper_cleanup(&nvme_ctx);
        return 1;
    }
    
    // Initialize concurrency manager
    if (concurrency_manager_init(&concurrency_mgr, &nvme_ctx, config.num_threads, config.queue_depth) != 0) {
        fprintf(stderr, "Failed to initialize concurrency manager\n");
        volume_manager_cleanup(&vol_mgr);
        nvme_wrapper_cleanup(&nvme_ctx);
        return 1;
    }
    
    // Start worker threads
    if (concurrency_manager_start(&concurrency_mgr) != 0) {
        fprintf(stderr, "Failed to start worker threads\n");
        concurrency_manager_cleanup(&concurrency_mgr);
        volume_manager_cleanup(&vol_mgr);
        nvme_wrapper_cleanup(&nvme_ctx);
        return 1;
    }
    
    if (config.verbose) {
        printf("Starting workload with %u threads, queue depth %u\n",
               config.num_threads, config.queue_depth);
    }
    
    // Run workload
    struct timeval start_tv, current_tv;
    gettimeofday(&start_tv, NULL);
    uint64_t start_time_us = (uint64_t)start_tv.tv_sec * 1000000 + start_tv.tv_usec;
    uint64_t operations_submitted = 0;
    
    g_running = true;
    
    while (g_running) {
        // Check duration limit
        if (config.duration_sec > 0) {
            gettimeofday(&current_tv, NULL);
            uint64_t current_time_us = (uint64_t)current_tv.tv_sec * 1000000 + current_tv.tv_usec;
            uint64_t elapsed_us = current_time_us - start_time_us;
            if (elapsed_us >= (uint64_t)config.duration_sec * 1000000) {
                break;
            }
        }
        
        // Check operation count limit
        if (config.num_operations > 0 && operations_submitted >= config.num_operations) {
            break;
        }
        
        // Generate ranges for next operation
        struct copy_range_descriptor ranges[MAX_COPY_RANGES];
        uint32_t num_ranges = 0;
        
        if (range_generator_next(&range_gen, ranges, MAX_COPY_RANGES, &num_ranges) != 0) {
            // Reset generator if we've exhausted ranges
            range_generator_reset(&range_gen);
            continue;
        }
        
        if (num_ranges == 0) {
            // No more ranges, reset and continue
            range_generator_reset(&range_gen);
            continue;
        }
        
        // Build and submit operation
        struct xcopy_operation op;
        if (xcopy_cmd_build(&op, config.dst_nsid, ranges, num_ranges) != 0) {
            fprintf(stderr, "Failed to build command\n");
            continue;
        }
        
        // Submit operation
        if (concurrency_manager_submit(&concurrency_mgr, &op) == 0) {
            operations_submitted++;
        }
    }
    
    // Stop and wait for completion
    if (config.verbose) {
        printf("Stopping workload...\n");
    }
    
    concurrency_manager_stop(&concurrency_mgr);
    concurrency_manager_wait(&concurrency_mgr);
    
    // Get statistics
    concurrency_manager_get_stats(&concurrency_mgr, &stats);
    
    // Calculate throughput
    gettimeofday(&current_tv, NULL);
    uint64_t end_time_us = (uint64_t)current_tv.tv_sec * 1000000 + current_tv.tv_usec;
    double elapsed_sec = (double)(end_time_us - start_time_us) / 1000000.0;
    stats.throughput_mbps = statistics_calculate_throughput(&stats, elapsed_sec);
    
    // Print statistics
    printf("\n=== Workload Complete ===\n");
    printf("Duration: %.2f seconds\n", elapsed_sec);
    printf("Operations submitted: %llu\n", (unsigned long long)operations_submitted);
    printf("Operations completed: %llu\n", (unsigned long long)stats.operations_completed);
    printf("Operations failed: %llu\n", (unsigned long long)stats.operations_failed);
    printf("Bytes copied: %llu (%.2f MB)\n", (unsigned long long)stats.bytes_copied,
           (double)stats.bytes_copied / (1024.0 * 1024.0));
    
    if (stats.operations_completed > 0) {
        double avg_latency_us = (double)stats.total_latency_us / stats.operations_completed;
        printf("Average latency: %.2f us\n", avg_latency_us);
        printf("Min latency: %llu us\n", (unsigned long long)stats.min_latency_us);
        printf("Max latency: %llu us\n", (unsigned long long)stats.max_latency_us);
    }
    
    printf("Throughput: %.2f MB/s\n", stats.throughput_mbps);
    printf("========================\n");
    
    // Cleanup
    concurrency_manager_cleanup(&concurrency_mgr);
    volume_manager_cleanup(&vol_mgr);
    nvme_wrapper_cleanup(&nvme_ctx);
    
    return rc;
}

