#include "spdk_context.h"
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstdlib>

// SPDK thread library is already declared in spdk/thread.h (included via spdk_context.h)

namespace xload {

// Helper function to get hugepage info from /proc/meminfo
static void print_hugepage_info(const char* prefix) {
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) {
        std::cerr << prefix << "Could not read /proc/meminfo" << std::endl;
        return;
    }
    
    std::string line;
    uint64_t hugepages_total = 0, hugepages_free = 0, hugepage_size_kb = 0;
    
    while (std::getline(meminfo, line)) {
        if (line.find("HugePages_Total:") != std::string::npos) {
            std::istringstream iss(line);
            std::string key;
            iss >> key >> hugepages_total;
        } else if (line.find("HugePages_Free:") != std::string::npos) {
            std::istringstream iss(line);
            std::string key;
            iss >> key >> hugepages_free;
        } else if (line.find("Hugepagesize:") != std::string::npos) {
            std::istringstream iss(line);
            std::string key;
            iss >> key >> hugepage_size_kb;
        }
    }
    
    uint64_t total_mb = (hugepages_total * hugepage_size_kb) / 1024;
    uint64_t free_mb = (hugepages_free * hugepage_size_kb) / 1024;
    
    std::cout << prefix << "Hugepages: " << hugepages_free << "/" << hugepages_total 
              << " free (" << free_mb << "/" << total_mb << " MB), page_size=" 
              << hugepage_size_kb << " KB" << std::endl;
}

// Flag to control threading mode - can be set before init()
static bool g_use_spdk_threads = true;  // Default: try to use SPDK threads

SpdkContext::SpdkContext()
    : ctrlr_(nullptr)
    , initialized_(false)
    , hostnqn_("")
    , main_thread_(nullptr)
{
    memset(&trid_, 0, sizeof(trid_));
}

SpdkContext::~SpdkContext() {
    cleanup();
}

bool SpdkContext::probe_cb(void* cb_ctx, const struct spdk_nvme_transport_id* trid,
                           struct spdk_nvme_ctrlr_opts* opts) {
    SpdkContext* ctx = static_cast<SpdkContext*>(cb_ctx);
    
    // Copy transport ID
    memcpy(&ctx->trid_, trid, sizeof(*trid));
    
    // Set controller options
    if (opts) {
        opts->opts_size = sizeof(*opts);
        
        // Set Host NQN if provided (overrides environment variable)
        if (!ctx->hostnqn_.empty() && ctx->hostnqn_.length() < sizeof(opts->hostnqn)) {
            strncpy(opts->hostnqn, ctx->hostnqn_.c_str(), sizeof(opts->hostnqn) - 1);
            opts->hostnqn[sizeof(opts->hostnqn) - 1] = '\0';
        }
    }
    
    return true;
}

void SpdkContext::attach_cb(void* cb_ctx, const struct spdk_nvme_transport_id* trid,
                            struct spdk_nvme_ctrlr* ctrlr,
                            const struct spdk_nvme_ctrlr_opts* opts) {
    (void)trid;
    (void)opts;
    
    SpdkContext* ctx = static_cast<SpdkContext*>(cb_ctx);
    ctx->ctrlr_ = ctrlr;
}

int SpdkContext::init(const std::string& traddr, const std::string& trsvcid,
                      const std::string& hostnqn, const std::string& subnqn) {
    if (initialized_) {
        return 0;
    }
    
    // Store hostnqn for use in probe callback
    hostnqn_ = hostnqn;
    
    // Set hostnqn via environment variable BEFORE spdk_env_init()
    // This ensures SPDK reads the correct Host NQN during initialization
    // However, we'll also set it in controller options to ensure it's used
    if (!hostnqn.empty()) {
        setenv("SPDK_NVME_HOSTNQN", hostnqn.c_str(), 1);
    }
    
    // Initialize SPDK environment with minimal options (matching SPDK tools)
    struct spdk_env_opts opts;
    spdk_env_opts_init(&opts);
    opts.name = "x-load";
    
    // Use unique shared memory ID to avoid conflicts with other DPDK/SPDK processes
    // -1 means auto-generate a unique ID based on PID
    opts.shm_id = -1;
    
    // Don't limit memory - let SPDK use what it needs from available hugepages
    // The thread library mempool needs memory from DPDK's pool
    // opts.mem_size is left at default (-1 = use all available)
    
    // Print hugepage info before SPDK init
    print_hugepage_info("[Pre-init] ");
    
    // Initialize environment
    std::cout << "Initializing SPDK environment (shm_id=" << opts.shm_id << ")..." << std::endl;
    if (spdk_env_init(&opts) < 0) {
        std::cerr << "Failed to initialize SPDK environment" << std::endl;
        std::cerr << "Hint: Check hugepages availability with 'cat /proc/meminfo | grep HugePages'" << std::endl;
        return -1;
    }
    std::cout << "SPDK environment initialized successfully" << std::endl;
    
    // Print hugepage info after SPDK env init
    print_hugepage_info("[Post-env-init] ");
    
    // Initialize SPDK thread library with progressive mempool size attempts
    // The mempool is allocated from DPDK hugepages, so we need to find a size that fits
    // Try progressively smaller sizes until one works
    if (g_use_spdk_threads) {
        // Mempool sizes to try (from largest to smallest)
        // Each message entry uses ~128 bytes, so:
        // 65536 = ~8MB, 32768 = ~4MB, 16384 = ~2MB, 8192 = ~1MB, 
        // 4096 = ~512KB, 2048 = ~256KB, 1024 = ~128KB, 512 = ~64KB
        const size_t mempool_sizes[] = {65536, 32768, 16384, 8192, 4096, 2048, 1024, 512};
        const int num_sizes = sizeof(mempool_sizes) / sizeof(mempool_sizes[0]);
        int rc = -1;
        
        for (int i = 0; i < num_sizes; i++) {
            size_t msg_size = mempool_sizes[i];
            rc = spdk_thread_lib_init_ext(nullptr, nullptr, 0, msg_size);
            if (rc == 0) {
                std::cout << "SPDK thread library initialized with mempool_size=" << msg_size << std::endl;
                break;
            } else {
                std::cerr << "spdk_thread_lib_init_ext(msg_size=" << msg_size << ") failed (rc=" << rc << ")" << std::endl;
            }
        }
        
        if (rc != 0) {
            // Try the simple initialization (uses default mempool size)
            std::cerr << "All extended init attempts failed, trying simple init..." << std::endl;
            rc = spdk_thread_lib_init(nullptr, 0);
            if (rc != 0) {
                std::cerr << "spdk_thread_lib_init also failed (rc=" << rc << ")" << std::endl;
                std::cerr << "Falling back to single-threaded mode" << std::endl;
                print_hugepage_info("[After thread init failure] ");
                g_use_spdk_threads = false;
            }
        }
    }
    
    if (g_use_spdk_threads) {
        std::cout << "Multi-threaded mode enabled" << std::endl;
        main_thread_ = nullptr;  // Will be set per-thread in PollThreadManager
    } else {
        main_thread_ = nullptr;
        std::cout << "Running in single-threaded mode (SPDK thread library not available)" << std::endl;
    }
    
    // Initialize transport ID for NVMe/TCP
    memset(&trid_, 0, sizeof(trid_));
    trid_.trtype = SPDK_NVME_TRANSPORT_TCP;
    trid_.adrfam = SPDK_NVMF_ADRFAM_IPV4;  // Set address family to IPv4
    
    if (traddr.length() >= sizeof(trid_.traddr)) {
        std::cerr << "Transport address too long" << std::endl;
        return -1;
    }
    strncpy(trid_.traddr, traddr.c_str(), sizeof(trid_.traddr) - 1);
    
    if (trsvcid.length() >= sizeof(trid_.trsvcid)) {
        std::cerr << "Service ID too long" << std::endl;
        return -1;
    }
    strncpy(trid_.trsvcid, trsvcid.c_str(), sizeof(trid_.trsvcid) - 1);
    
    if (!subnqn.empty() && subnqn.length() < sizeof(trid_.subnqn)) {
        strncpy(trid_.subnqn, subnqn.c_str(), sizeof(trid_.subnqn) - 1);
    }
    
    // Check if TCP transport is available
    const char* tcp_name = spdk_nvme_transport_id_trtype_str(SPDK_NVME_TRANSPORT_TCP);
    if (!tcp_name || strcmp(tcp_name, "Unknown") == 0) {
        std::cerr << "Error: NVMe/TCP transport is not available" << std::endl;
        std::cerr << "Note: Ensure SPDK was built with NVMe/TCP transport support" << std::endl;
        return -1;
    }
    
    // Check if transport is registered/available
    // In SPDK, transports should be auto-registered, but we'll verify
    if (!spdk_nvme_transport_available(SPDK_NVME_TRANSPORT_TCP)) {
        std::cerr << "Error: NVMe/TCP transport is not registered" << std::endl;
        std::cerr << "Note: The transport may need to be explicitly registered" << std::endl;
        std::cerr << "      or SPDK may need to be rebuilt with TCP support" << std::endl;
        return -1;
    }
    
    // Probe and attach using standard API
    if (spdk_nvme_probe(&trid_, this, probe_cb, attach_cb, nullptr) != 0) {
        std::cerr << "Failed to probe for NVMe controllers" << std::endl;
        std::cerr << "Transport: " << tcp_name << std::endl;
        std::cerr << "Address: " << traddr << ":" << trsvcid << std::endl;
        if (!subnqn.empty()) {
            std::cerr << "Subsystem NQN: " << subnqn << std::endl;
        }
        return -1;
    }
    
    if (!ctrlr_) {
        std::cerr << "No NVMe controller found" << std::endl;
        return -1;
    }
    
    // Discover namespaces
    discover_namespaces();
    
    initialized_ = true;
    return 0;
}

void SpdkContext::discover_namespaces() {
    namespaces_.clear();
    
    if (!ctrlr_) {
        return;
    }
    
    // Iterate through all namespaces
    for (uint32_t nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr_);
         nsid != 0;
         nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr_, nsid)) {
        
        struct spdk_nvme_ns* ns = spdk_nvme_ctrlr_get_ns(ctrlr_, nsid);
        if (!ns) {
            continue;
        }
        
        NamespaceInfo info;
        info.nsid = nsid;
        info.ns = ns;
        info.size_blocks = spdk_nvme_ns_get_num_sectors(ns);
        info.sector_size = spdk_nvme_ns_get_sector_size(ns);
        info.block_size = info.sector_size;
        
        namespaces_.push_back(info);
    }
}

struct spdk_nvme_ns* SpdkContext::get_ns(uint32_t nsid) const {
    if (!ctrlr_) {
        return nullptr;
    }
    return spdk_nvme_ctrlr_get_ns(ctrlr_, nsid);
}

const NamespaceInfo* SpdkContext::get_ns_info(uint32_t nsid) const {
    for (const auto& info : namespaces_) {
        if (info.nsid == nsid) {
            return &info;
        }
    }
    return nullptr;
}

bool SpdkContext::supports_cross_namespace_copy() const {
    if (!ctrlr_) {
        return false;
    }
    
    // Check if controller supports Simple Copy Command (SCC)
    // Check for TP4130 support (cross-namespace copy)
    // This is indicated by the SCCS (Simple Copy Command Support) bit
    // and the ability to specify different source NSIDs in copy range descriptors
    // For now, we'll assume support if SCC is available
    // A more thorough check would examine the Identify Controller data structure
    // const struct spdk_nvme_ctrlr_data* cdata = spdk_nvme_ctrlr_get_data(ctrlr_);
    
    return true; // Simplified - should check actual controller capabilities
}

struct spdk_nvme_qpair* SpdkContext::create_qpair(uint32_t queue_depth,
                                                   spdk_nvme_io_qpair_opts* opts) {
    if (!ctrlr_) {
        return nullptr;
    }
    
    spdk_nvme_io_qpair_opts default_opts;
    if (!opts) {
        spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr_, &default_opts, sizeof(default_opts));
        opts = &default_opts;
    }
    
    opts->qprio = SPDK_NVME_QPRIO_URGENT;
    opts->io_queue_size = queue_depth;
    
    return spdk_nvme_ctrlr_alloc_io_qpair(ctrlr_, opts, sizeof(*opts));
}

void SpdkContext::delete_qpair(struct spdk_nvme_qpair* qpair) {
    if (qpair && ctrlr_) {
        spdk_nvme_ctrlr_free_io_qpair(qpair);
    }
}

void SpdkContext::cleanup() {
    if (ctrlr_) {
        spdk_nvme_detach(ctrlr_);
        ctrlr_ = nullptr;
    }
    
    namespaces_.clear();
    initialized_ = false;
    
    // Cleanup SPDK thread library if it was initialized
    if (g_use_spdk_threads) {
        spdk_thread_lib_fini();
    }
}

bool SpdkContext::is_spdk_threads_enabled() {
    return g_use_spdk_threads;
}

void SpdkContext::set_spdk_threads_enabled(bool enabled) {
    g_use_spdk_threads = enabled;
}

} // namespace xload

