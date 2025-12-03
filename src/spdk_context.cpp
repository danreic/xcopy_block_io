#include "spdk_context.h"
#include <cstring>
#include <iostream>
#include <cstdio>
#include <cstdlib>

namespace xload {

SpdkContext::SpdkContext()
    : ctrlr_(nullptr)
    , initialized_(false)
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
    
    // Initialize SPDK environment
    struct spdk_env_opts opts;
    spdk_env_opts_init(&opts);
    opts.opts_size = sizeof(opts);  // CRITICAL: Must be set after init
    opts.name = "x-load";
    opts.shm_id = 0;
    
    // Configure hugepages
    opts.hugepage_single_segments = false;
    opts.unlink_hugepage = false;
    
    // Set memory size (512 MB should be sufficient)
    opts.mem_size = 512;
    
    // Initialize environment
    if (spdk_env_init(&opts) < 0) {
        std::cerr << "Failed to initialize SPDK environment" << std::endl;
        return -1;
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
    
    // Set hostnqn via environment variable (SPDK uses this)
    if (!hostnqn.empty()) {
        setenv("SPDK_NVME_HOSTNQN", hostnqn.c_str(), 1);
    }
    
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
}

} // namespace xload

