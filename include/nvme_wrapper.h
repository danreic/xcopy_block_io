#ifndef NVME_WRAPPER_H
#define NVME_WRAPPER_H

#include <stdint.h>
#include <stdbool.h>
#include <libnvme.h>
#include <sys/types.h>

// Forward declarations
struct nvme_ctrl;
struct nvme_ns;

// Transport types (keeping same enum for compatibility)
typedef enum {
    XCOPY_TRANSPORT_PCIE,
    XCOPY_TRANSPORT_TCP,
    XCOPY_TRANSPORT_RDMA
} xcopy_transport_type_t;

// Transport configuration
struct xcopy_transport_config {
    xcopy_transport_type_t type;
    char *traddr;              // Transport address
    char *trsvcid;             // Transport service ID
    char *subnqn;              // Subsystem NQN
    char *hostnqn;             // Host NQN (optional)
};

// NVMe context (replaces spdk_context)
struct nvme_context {
    struct nvme_ctrl *ctrl;    // libnvme controller
    struct nvme_ns *ns_list;   // List of namespaces
    uint32_t num_ns;           // Number of namespaces
    bool initialized;
    bool connected;
};

// Initialize NVMe context
int nvme_wrapper_init(struct nvme_context *ctx,
                     struct xcopy_transport_config *transport);

// Connect to NVMe controller
int nvme_wrapper_connect(struct nvme_context *ctx,
                        struct xcopy_transport_config *transport);

// Get controller (returns libnvme controller pointer)
struct nvme_ctrl *nvme_wrapper_get_ctrl(struct nvme_context *ctx);

// Get namespace by ID
struct nvme_ns *nvme_wrapper_get_ns(struct nvme_context *ctx, uint32_t nsid);

// Get namespace size in blocks
uint64_t nvme_wrapper_get_ns_size(struct nvme_context *ctx, uint32_t nsid);

// Get block size
uint32_t nvme_wrapper_get_block_size(struct nvme_context *ctx, uint32_t nsid);

// Cleanup NVMe context
void nvme_wrapper_cleanup(struct nvme_context *ctx);

// Parse transport type from string
xcopy_transport_type_t nvme_wrapper_parse_transport(const char *transport_str);

// Submit passthrough command (for XCOPY)
int nvme_wrapper_submit_passthru(struct nvme_context *ctx,
                                 uint32_t nsid,
                                 struct nvme_passthru_cmd *cmd,
                                 void *data,
                                 size_t data_len);

#endif // NVME_WRAPPER_H

