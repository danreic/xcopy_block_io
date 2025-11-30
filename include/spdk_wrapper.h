#ifndef SPDK_WRAPPER_H
#define SPDK_WRAPPER_H

#include <stdint.h>
#include <stdbool.h>
#include <spdk/nvme.h>
#include <spdk/env.h>

// Transport types
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

// SPDK context
struct spdk_context {
    struct spdk_env_opts opts;
    struct spdk_nvme_ctrlr *ctrlr;
    struct spdk_nvme_transport_id trid;
    bool initialized;
};

// Initialize SPDK environment
int spdk_wrapper_init(struct spdk_context *ctx,
                     struct xcopy_transport_config *transport);

// Probe and attach to NVMe controller
int spdk_wrapper_probe_attach(struct spdk_context *ctx,
                             struct xcopy_transport_config *transport);

// Get controller
struct spdk_nvme_ctrlr *spdk_wrapper_get_ctrlr(struct spdk_context *ctx);

// Get namespace by ID
struct spdk_nvme_ns *spdk_wrapper_get_ns(struct spdk_context *ctx, uint32_t nsid);

// Get namespace size in blocks
uint64_t spdk_wrapper_get_ns_size(struct spdk_context *ctx, uint32_t nsid);

// Get block size
uint32_t spdk_wrapper_get_block_size(struct spdk_context *ctx, uint32_t nsid);

// Cleanup SPDK context
void spdk_wrapper_cleanup(struct spdk_context *ctx);

// Parse transport type from string
xcopy_transport_type_t spdk_wrapper_parse_transport(const char *transport_str);

#endif // SPDK_WRAPPER_H

