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
    
    // Initialize SPDK environment
    int init(const std::string& traddr, const std::string& trsvcid,
             const std::string& hostnqn, const std::string& subnqn);
    
    // Cleanup
    void cleanup();
    
    // Get controller
    struct spdk_nvme_ctrlr* get_ctrlr() const { return ctrlr_; }
    
    // Get namespace by ID
    struct spdk_nvme_ns* get_ns(uint32_t nsid) const;
    
    // Get namespace info
    const NamespaceInfo* get_ns_info(uint32_t nsid) const;
    
    // Get all namespaces
    const std::vector<NamespaceInfo>& get_namespaces() const { return namespaces_; }
    
    // Check if TP4130 (cross-namespace copy) is supported
    bool supports_cross_namespace_copy() const;
    
    // Create a QPair for a specific thread
    struct spdk_nvme_qpair* create_qpair(uint32_t queue_depth, 
                                         spdk_nvme_io_qpair_opts* opts = nullptr);
    
    // Delete a QPair
    void delete_qpair(struct spdk_nvme_qpair* qpair);
    
    // Check if initialized
    bool is_initialized() const { return initialized_; }
    
private:
    struct spdk_nvme_ctrlr* ctrlr_;
    struct spdk_nvme_transport_id trid_;
    std::vector<NamespaceInfo> namespaces_;
    bool initialized_;
    std::string hostnqn_;  // Store hostnqn for use in probe callback
    
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

