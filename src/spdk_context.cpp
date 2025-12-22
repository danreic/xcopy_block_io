#include "spdk_context.h"
#include <cstring>
#include <iostream>
#include <cstdio>
#include <cstdlib>

// SPDK thread library is already declared in spdk/thread.h (included via spdk_context.h)

namespace xload {

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
    
    // mem_size is in MB, -1 means use all available (default)
    // Set to 512MB to have room for:
    // - DPDK EAL infrastructure (~128MB)
    // - NVMe driver buffers (~64MB)  
    // - Thread library mempool (~256MB for 8192 messages)
    opts.mem_size = 512;
    
    // Initialize environment
    std::cout << "Initializing SPDK environment with " << opts.mem_size << " MB memory limit..." << std::endl;
    if (spdk_env_init(&opts) < 0) {
        std::cerr << "Failed to initialize SPDK environment" << std::endl;
        std::cerr << "Hint: Check hugepages availability with 'cat /proc/meminfo | grep HugePages'" << std::endl;
        return -1;
    }
    std::cout << "SPDK environment initialized successfully" << std::endl;
    
    // Initialize SPDK thread library
    // We need this for multi-threaded operation and proper completion handling
    if (g_use_spdk_threads) {
        // Try the extended version first (has configurable mempool size)
        // Use smaller mempool size (8192 messages) to conserve hugepage memory
        // Signature: spdk_thread_lib_init_ext(thread_op_fn, thread_op_supported_fn, ctx_sz, msg_mempool_size)
        int rc = spdk_thread_lib_init_ext(nullptr, nullptr, 0, 8192);
        if (rc != 0) {
            std::cerr << "Warning: spdk_thread_lib_init_ext failed (rc=" << rc 
                      << "), trying simple init..." << std::endl;
            
            // Fall back to simple initialization
            rc = spdk_thread_lib_init(nullptr, 0);
            if (rc != 0) {
                std::cerr << "Warning: spdk_thread_lib_init also failed (rc=" << rc 
                          << "), falling back to workaround mode" << std::endl;
                g_use_spdk_threads = false;
            }
        }
    }
    
    if (g_use_spdk_threads) {
        std::cout << "SPDK thread library initialized (multi-threaded mode enabled)" << std::endl;
        main_thread_ = nullptr;  // Will be set per-thread in PollThreadManager
    } else {
        // WORKAROUND: Skip SPDK thread creation entirely
        // We'll use a single-threaded model without SPDK threads
        // This bypasses the thread library initialization issue completely
        main_thread_ = nullptr;
        std::cout << "Note: Running in single-threaded mode (workaround mode)" << std::endl;
        std::cout << "      SPDK threads disabled - using direct I/O submission" << std::endl;
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

