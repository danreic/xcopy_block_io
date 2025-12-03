#include "config_manager.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <algorithm>

namespace xload {

Config::Config()
    : traddr("")
    , trsvcid("4420")
    , hostnqn("")
    , subnqn("")
    , runtime_sec(10)
    , iodepth(64)
    , num_cores(1)
    , max_ranges(1)
    , dst_nsid(1)
    , dst_lba_start(0)
    , dst_lba_end(0)
    , range_size(2048)  // Default: 2048 blocks = 1MB at 512 bytes/block
    , enable_cross_namespace(false)  // Default: same-namespace copy only (format 0)
    , json_output(false)
    , verbose(false)
{
}

bool Config::validate() const {
    if (traddr.empty()) {
        std::cerr << "Error: --traddr is required" << std::endl;
        return false;
    }
    
    if (iodepth == 0) {
        std::cerr << "Error: --iodepth must be > 0" << std::endl;
        return false;
    }
    
    if (num_cores == 0) {
        std::cerr << "Error: --num-cores must be > 0" << std::endl;
        return false;
    }
    
    if (max_ranges == 0 || max_ranges > 16) {
        std::cerr << "Error: --max-ranges must be between 1 and 16" << std::endl;
        return false;
    }
    
    if (dst_nsid == 0) {
        std::cerr << "Error: --dst-nsid must be > 0" << std::endl;
        return false;
    }
    
    if (range_size == 0) {
        std::cerr << "Error: --range-size must be > 0" << std::endl;
        return false;
    }
    
    return true;
}

void Config::print() const {
    std::cout << "Configuration:" << std::endl;
    std::cout << "  Target Address: " << traddr << std::endl;
    std::cout << "  Service ID: " << trsvcid << std::endl;
    if (!hostnqn.empty()) {
        std::cout << "  Host NQN: " << hostnqn << std::endl;
    }
    if (!subnqn.empty()) {
        std::cout << "  Subsystem NQN: " << subnqn << std::endl;
    }
    std::cout << "  Runtime: " << runtime_sec << " seconds" << std::endl;
    std::cout << "  I/O Depth: " << iodepth << std::endl;
    std::cout << "  CPU Cores: " << num_cores << std::endl;
    std::cout << "  Max Ranges: " << max_ranges << std::endl;
    std::cout << "  Range Size: " << range_size << " blocks" << std::endl;
    std::cout << "  Destination NSID: " << dst_nsid << std::endl;
    if (!src_nsids.empty()) {
        std::cout << "  Source NSIDs: ";
        for (size_t i = 0; i < src_nsids.size(); i++) {
            if (i > 0) std::cout << ", ";
            std::cout << src_nsids[i];
        }
        std::cout << std::endl;
    }
    std::cout << "  Cross-Namespace Copy: " << (enable_cross_namespace ? "enabled" : "disabled") << std::endl;
    std::cout << "  Destination LBA: " << dst_lba_start;
    if (dst_lba_end > 0) {
        std::cout << " - " << dst_lba_end;
    }
    std::cout << std::endl;
    std::cout << "  Range Size: " << range_size << " blocks" << std::endl;
    std::cout << "  JSON Output: " << (json_output ? "yes" : "no") << std::endl;
    std::cout << "  Verbose: " << (verbose ? "yes" : "no") << std::endl;
}

void ConfigManager::print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [OPTIONS]" << std::endl;
    std::cout << std::endl;
    std::cout << "Required Options:" << std::endl;
    std::cout << "  --traddr ADDR         Target IP address (required)" << std::endl;
    std::cout << "  --trsvcid PORT        Service ID/port (default: 4420)" << std::endl;
    std::cout << "  --hostnqn NQN         Host NQN (required)" << std::endl;
    std::cout << std::endl;
    std::cout << "Workload Options:" << std::endl;
    std::cout << "  --runtime SEC         Runtime duration in seconds (default: 10, 0 = infinite)" << std::endl;
    std::cout << "  --iodepth DEPTH       Maximum global I/O depth (default: 64)" << std::endl;
    std::cout << "  --num-cores CORES     Number of dedicated CPU cores (default: 1)" << std::endl;
    std::cout << "  --max-ranges NUM      Maximum source ranges per command (1-16, default: 1)" << std::endl;
    std::cout << "  --dst-nsid NSID       Destination namespace ID (default: 1)" << std::endl;
    std::cout << "  --src-nsid NSID       Source namespace ID (can be specified multiple times)" << std::endl;
    std::cout << "  --dst-lba-start LBA   Starting LBA for destination (default: 0)" << std::endl;
    std::cout << "  --dst-lba-end LBA     Ending LBA for destination (default: 0 = use namespace size)" << std::endl;
    std::cout << "  --range-size SIZE     Size of each range in blocks (default: 2048 = 1MB at 512B/block)" << std::endl;
    std::cout << "  --enable-cross-ns     Enable cross-namespace copy (format 2, requires TP4130 support)" << std::endl;
    std::cout << std::endl;
    std::cout << "Output Options:" << std::endl;
    std::cout << "  --json                Output statistics in JSON format" << std::endl;
    std::cout << "  -v, --verbose         Verbose output" << std::endl;
    std::cout << "  -h, --help            Show this help message" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << prog_name << " --traddr 192.168.1.100 --hostnqn nqn.2014-08.org.nvmexpress:uuid:1234 --runtime 60 --iodepth 256 --num-cores 4" << std::endl;
}

int ConfigManager::parse_args(int argc, char** argv, Config& config) {
    static struct option long_options[] = {
        {"traddr", required_argument, 0, 'a'},
        {"trsvcid", required_argument, 0, 's'},
        {"hostnqn", required_argument, 0, 'n'},
        {"subnqn", required_argument, 0, 1000},
        {"runtime", required_argument, 0, 'r'},
        {"iodepth", required_argument, 0, 'd'},
        {"num-cores", required_argument, 0, 'c'},
        {"max-ranges", required_argument, 0, 1001},
        {"dst-nsid", required_argument, 0, 1002},
        {"src-nsid", required_argument, 0, 1003},
        {"dst-lba-start", required_argument, 0, 1004},
        {"dst-lba-end", required_argument, 0, 1005},
        {"range-size", required_argument, 0, 1006},
        {"enable-cross-ns", no_argument, 0, 1007},
        {"json", no_argument, 0, 'j'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    int option_index = 0;
    
    while ((opt = getopt_long(argc, argv, "a:s:n:r:d:c:jvh", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'a':
                config.traddr = optarg;
                break;
            case 's':
                config.trsvcid = optarg;
                break;
            case 'n':
                config.hostnqn = optarg;
                break;
            case 1000:
                config.subnqn = optarg;
                break;
            case 'r':
                config.runtime_sec = strtoull(optarg, nullptr, 0);
                break;
            case 'd':
                config.iodepth = strtoul(optarg, nullptr, 0);
                break;
            case 'c':
                config.num_cores = strtoul(optarg, nullptr, 0);
                break;
            case 1001:
                config.max_ranges = strtoul(optarg, nullptr, 0);
                break;
            case 1002:
                config.dst_nsid = strtoul(optarg, nullptr, 0);
                break;
            case 1003:
                config.src_nsids.push_back(strtoul(optarg, nullptr, 0));
                break;
            case 1004:
                config.dst_lba_start = strtoull(optarg, nullptr, 0);
                break;
            case 1005:
                config.dst_lba_end = strtoull(optarg, nullptr, 0);
                break;
            case 1006:
                config.range_size = strtoull(optarg, nullptr, 0);
                break;
            case 1007:
                config.enable_cross_namespace = true;
                break;
            case 'j':
                config.json_output = true;
                break;
            case 'v':
                config.verbose = true;
                break;
            case 'h':
                print_usage(argv[0]);
                return 1;
            default:
                print_usage(argv[0]);
                return -1;
        }
    }
    
    // If no source NSIDs specified, use destination NSID as default
    if (config.src_nsids.empty()) {
        config.src_nsids.push_back(config.dst_nsid);
    }
    
    if (!config.validate()) {
        return -1;
    }
    
    return 0;
}

} // namespace xload

