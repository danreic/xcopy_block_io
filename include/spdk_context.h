#ifndef SPDK_CONTEXT_H
#define SPDK_CONTEXT_H

#include <spdk/nvme.h>
#include <spdk/nvmf_spec.h>
#include <spdk/env.h>
#include <spdk/thread.h>
#include <stdint.h>
#include <vector>
#include <string>
#include <memory>

namespace xload {

struct NamespaceInfo {
    uint32_t nsid;
    struct spdk_nvme_ns* ns;
    uint64_t size_blocks;
    uint32_t block_size;
    uint32_t sector_size;
};

class SpdkContext {
public:
    SpdkContext();
    ~SpdkContext();
    
    // Initialize SPDK environment with multiple target addresses (multi-path)
    int init(const std::vector<std::string>& traddrs, const std::string& trsvcid,
             const std::string& hostnqn, const std::string& subnqn);
    
    // Cleanup
    void cleanup();
    
    // Get controller count (multi-path)
    size_t get_ctrlr_count() const { return ctrlrs_.size(); }
    
    // Get controller by index
    struct spdk_nvme_ctrlr* get_ctrlr(size_t index = 0) const { 
        return index < ctrlrs_.size() ? ctrlrs_[index] : nullptr; 
    }
    
    // Get namespace by ID (uses first controller - all controllers have same namespaces)
    struct spdk_nvme_ns* get_ns(uint32_t nsid) const;
    
    // Get namespace info
    const NamespaceInfo* get_ns_info(uint32_t nsid) const;
    
    // Get all namespaces
    const std::vector<NamespaceInfo>& get_namespaces() const { return namespaces_; }
    
    // Check if TP4130 (cross-namespace copy) is supported
    bool supports_cross_namespace_copy() const;
    
    // Create a QPair for a specific controller (for multi-path load balancing)
    struct spdk_nvme_qpair* create_qpair(uint32_t queue_depth, 
                                         spdk_nvme_io_qpair_opts* opts = nullptr,
                                         size_t ctrlr_index = 0);
    
    // Delete a QPair
    void delete_qpair(struct spdk_nvme_qpair* qpair);
    
    // Check if initialized
    bool is_initialized() const { return initialized_; }
    
    // Get main SPDK thread (for workaround)
    struct spdk_thread* get_main_thread() const { return main_thread_; }
    
    // Check if SPDK threads are enabled (multi-threaded mode)
    static bool is_spdk_threads_enabled();
    
    // Enable/disable SPDK threads (call before init())
    static void set_spdk_threads_enabled(bool enabled);
    
    // Global verbose flag
    static void set_verbose(bool verbose);
    static bool is_verbose();
    
private:
    std::vector<struct spdk_nvme_ctrlr*> ctrlrs_;  // Multiple controllers for multi-path
    std::vector<struct spdk_nvme_transport_id> trids_;
    std::vector<NamespaceInfo> namespaces_;
    bool initialized_;
    std::string hostnqn_;  // Store hostnqn for use in probe callback
    struct spdk_thread* main_thread_;  // Main SPDK thread (workaround)
    
    // Current controller being attached (for callback)
    struct spdk_nvme_ctrlr* current_ctrlr_;
    
    // Probe callback
    static bool probe_cb(void* cb_ctx, const struct spdk_nvme_transport_id* trid,
                         struct spdk_nvme_ctrlr_opts* opts);
    
    // Attach callback
    static void attach_cb(void* cb_ctx, const struct spdk_nvme_transport_id* trid,
                          struct spdk_nvme_ctrlr* ctrlr,
                          const struct spdk_nvme_ctrlr_opts* opts);
    
    // Discover namespaces
    void discover_namespaces();
};

} // namespace xload

#endif // SPDK_CONTEXT_H

