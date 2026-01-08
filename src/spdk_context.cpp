#include "spdk_context.h"
#include <cstring>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

// DPDK includes for mempool check
#include <rte_mempool.h>
#include <rte_errno.h>

// SPDK thread library is already declared in spdk/thread.h (included via spdk_context.h)

namespace xload {

// Flag to control threading mode - can be set before init()
static bool g_use_spdk_threads = true;  // Default: try to use SPDK threads
static bool g_verbose = false;  // Global verbose flag

// Check if DPDK mempool works (needed for SPDK thread library)
// Returns true if mempool works, false otherwise (pthread fallback will be used)
static bool check_dpdk_mempool() {
    // Try to create a simple mempool - this is what SPDK thread library needs
    rte_errno = 0;
    struct rte_mempool* test_pool = rte_mempool_create_empty(
        "test_pool", 256, 64, 0, 0, SOCKET_ID_ANY, 0);
    
    if (!test_pool) {
        // Mempool creation failed - will use pthread-based multi-threading instead
        return false;
    }
    
    // Set mempool ops and populate
    int rc = rte_mempool_set_ops_byname(test_pool, "ring_mp_mc", nullptr);
    if (rc != 0) {
        rte_mempool_free(test_pool);
        return false;
    }
    
    rc = rte_mempool_populate_default(test_pool);
    if (rc < 0) {
        rte_mempool_free(test_pool);
        return false;
    }
    
    rte_mempool_free(test_pool);
    return true;
}

SpdkContext::SpdkContext()
    : ctrlrs_()
    , trids_()
    , initialized_(false)
    , hostnqn_("")
    , main_thread_(nullptr)
    , current_ctrlr_(nullptr)
{
}

SpdkContext::~SpdkContext() {
    cleanup();
}

bool SpdkContext::probe_cb(void* cb_ctx, const struct spdk_nvme_transport_id* trid,
                           struct spdk_nvme_ctrlr_opts* opts) {
    SpdkContext* ctx = static_cast<SpdkContext*>(cb_ctx);
    (void)trid;
    
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
    ctx->current_ctrlr_ = ctrlr;  // Store for the caller to add to ctrlrs_
}

int SpdkContext::init(const std::vector<std::string>& traddrs, const std::string& trsvcid,
                      const std::string& hostnqn, const std::string& subnqn) {
    if (initialized_) {
        return 0;
    }
    
    if (traddrs.empty()) {
        std::cerr << "Error: No target addresses provided" << std::endl;
        return -1;
    }
    
    // Store hostnqn for use in probe callback
    hostnqn_ = hostnqn;
    
    // Set hostnqn via environment variable BEFORE spdk_env_init()
    if (!hostnqn.empty()) {
        setenv("SPDK_NVME_HOSTNQN", hostnqn.c_str(), 1);
    }
    
    // Initialize SPDK environment with options configured for container/thread pool use
    struct spdk_env_opts opts;
    spdk_env_opts_init(&opts);
    opts.name = "x-load";
    
    // Request sufficient memory for DPDK heap allocation
    opts.mem_size = 512;
    
    // Container-friendly options:
    opts.no_pci = true;
    opts.env_context = const_cast<char*>("--no-telemetry");
    opts.shm_id = -1;
    
    // Auto-detect available cores: use only core 0 by default for container compatibility
    // DPDK will fail if we request cores that aren't available in the container/VM
    // The application handles multi-threading via pthread, so we only need 1 DPDK core
    opts.core_mask = "0x1";
    
    mkdir("/var/run/dpdk", 0777);
    
    if (spdk_env_init(&opts) < 0) {
        std::cerr << "Failed to initialize SPDK environment" << std::endl;
        std::cerr << "Hint: Check hugepages availability with 'cat /proc/meminfo | grep HugePages'" << std::endl;
        return -1;
    }
    if (g_verbose) {
        std::cout << "SPDK environment initialized" << std::endl;
    }
    
    // Check if DPDK mempool works (needed for SPDK thread library)
    bool mempool_works = check_dpdk_mempool();
    
    // Initialize SPDK thread library
    if (g_use_spdk_threads && mempool_works) {
        int rc = spdk_thread_lib_init_ext(nullptr, nullptr, 0, 1024);
        if (rc != 0) {
            rc = spdk_thread_lib_init_ext(nullptr, nullptr, 0, 256);
            if (rc != 0) {
                rc = spdk_thread_lib_init(nullptr, 0);
                if (rc != 0) {
                    g_use_spdk_threads = false;
                }
            }
        }
    } else if (g_use_spdk_threads && !mempool_works) {
        g_use_spdk_threads = false;
    }
    
    if (g_verbose) {
        if (g_use_spdk_threads) {
            std::cout << "SPDK thread library ENABLED" << std::endl;
        } else {
            std::cout << "Using pthread-based multi-threading" << std::endl;
        }
    }
    main_thread_ = nullptr;
    
    // Check if TCP transport is available
    const char* tcp_name = spdk_nvme_transport_id_trtype_str(SPDK_NVME_TRANSPORT_TCP);
    if (!tcp_name || strcmp(tcp_name, "Unknown") == 0) {
        std::cerr << "Error: NVMe/TCP transport is not available" << std::endl;
        return -1;
    }
    
    if (!spdk_nvme_transport_available(SPDK_NVME_TRANSPORT_TCP)) {
        std::cerr << "Error: NVMe/TCP transport is not registered" << std::endl;
        return -1;
    }
    
    // Connect to each target address (multi-path)
    std::cout << "Connecting to " << traddrs.size() << " target address(es)..." << std::endl;
    
    for (size_t i = 0; i < traddrs.size(); i++) {
        const std::string& traddr = traddrs[i];
        
        // Initialize transport ID for this address
        struct spdk_nvme_transport_id trid;
        memset(&trid, 0, sizeof(trid));
        trid.trtype = SPDK_NVME_TRANSPORT_TCP;
        trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
        
        if (traddr.length() >= sizeof(trid.traddr)) {
            std::cerr << "Transport address too long: " << traddr << std::endl;
            continue;
        }
        strncpy(trid.traddr, traddr.c_str(), sizeof(trid.traddr) - 1);
        
        if (trsvcid.length() >= sizeof(trid.trsvcid)) {
            std::cerr << "Service ID too long" << std::endl;
            continue;
        }
        strncpy(trid.trsvcid, trsvcid.c_str(), sizeof(trid.trsvcid) - 1);
        
        if (!subnqn.empty() && subnqn.length() < sizeof(trid.subnqn)) {
            strncpy(trid.subnqn, subnqn.c_str(), sizeof(trid.subnqn) - 1);
        }
        
        // Clear current controller before probe
        current_ctrlr_ = nullptr;
        
        // Probe and attach
        std::cout << "  [" << (i + 1) << "/" << traddrs.size() << "] " << traddr << ":" << trsvcid << "... " << std::flush;
        
        if (spdk_nvme_probe(&trid, this, probe_cb, attach_cb, nullptr) != 0) {
            std::cerr << "FAILED (probe error)" << std::endl;
            continue;
        }
        
        if (!current_ctrlr_) {
            std::cerr << "FAILED (no controller)" << std::endl;
            continue;
        }
        
        // Successfully connected - add to our list
        ctrlrs_.push_back(current_ctrlr_);
        trids_.push_back(trid);
        std::cout << "OK" << std::endl;
    }
    
    if (ctrlrs_.empty()) {
        std::cerr << "Error: Failed to connect to any target" << std::endl;
        return -1;
    }
    
    std::cout << "Connected to " << ctrlrs_.size() << " controller(s)" << std::endl;
    
    // Discover namespaces (from first controller - all should have same namespaces)
    discover_namespaces();
    
    initialized_ = true;
    return 0;
}

void SpdkContext::discover_namespaces() {
    namespaces_.clear();
    
    if (ctrlrs_.empty()) {
        return;
    }
    
    // Use first controller (all controllers have same namespaces for same subsystem)
    struct spdk_nvme_ctrlr* ctrlr = ctrlrs_[0];
    
    // Iterate through all namespaces
    for (uint32_t nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
         nsid != 0;
         nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
        
        struct spdk_nvme_ns* ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
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
    if (ctrlrs_.empty()) {
        return nullptr;
    }
    return spdk_nvme_ctrlr_get_ns(ctrlrs_[0], nsid);
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
    if (ctrlrs_.empty()) {
        return false;
    }
    
    // Check if controller supports Simple Copy Command (SCC)
    // For now, we'll assume support if SCC is available
    return true; // Simplified - should check actual controller capabilities
}

struct spdk_nvme_qpair* SpdkContext::create_qpair(uint32_t queue_depth,
                                                   spdk_nvme_io_qpair_opts* opts,
                                                   size_t ctrlr_index) {
    if (ctrlr_index >= ctrlrs_.size()) {
        return nullptr;
    }
    
    struct spdk_nvme_ctrlr* ctrlr = ctrlrs_[ctrlr_index];
    
    spdk_nvme_io_qpair_opts default_opts;
    if (!opts) {
        spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &default_opts, sizeof(default_opts));
        opts = &default_opts;
    }
    
    opts->qprio = SPDK_NVME_QPRIO_URGENT;
    opts->io_queue_size = queue_depth;
    
    return spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, opts, sizeof(*opts));
}

void SpdkContext::delete_qpair(struct spdk_nvme_qpair* qpair) {
    if (!qpair) {
        return;
    }
    // Need to find which controller owns this qpair
    // For simplicity, just free it - SPDK will handle it
    for (auto ctrlr : ctrlrs_) {
        if (ctrlr) {
            spdk_nvme_ctrlr_free_io_qpair(qpair);
            return;
        }
    }
}

void SpdkContext::cleanup() {
    // Detach all controllers
    for (auto ctrlr : ctrlrs_) {
        if (ctrlr) {
            spdk_nvme_detach(ctrlr);
        }
    }
    ctrlrs_.clear();
    trids_.clear();
    
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

void SpdkContext::set_verbose(bool verbose) {
    g_verbose = verbose;
}

bool SpdkContext::is_verbose() {
    return g_verbose;
}

} // namespace xload

