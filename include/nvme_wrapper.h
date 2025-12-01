#ifndef NVME_WRAPPER_H
#define NVME_WRAPPER_H

#include <stdint.h>
#include <stdbool.h>
#include <libnvme.h>
#include <sys/types.h>

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

// Namespace information
struct nvme_ns_info {
    uint32_t nsid;
    uint64_t size_blocks;
    uint32_t block_size;
    int fd;                    // File descriptor for this namespace
};

// NVMe context - uses libnvme's low-level API (file descriptors)
struct nvme_context {
    int ctrl_fd;               // Controller file descriptor (from nvme_open)
    char device_path[64];      // Device path (e.g., /dev/nvme0)
    struct nvme_ns_info *ns_list;  // List of namespaces
    uint32_t num_ns;           // Number of namespaces
    uint32_t ns_capacity;      // Capacity of ns_list array
    bool initialized;
    bool connected;
};

// Initialize NVMe context
int nvme_wrapper_init(struct nvme_context *ctx,
                     struct xcopy_transport_config *transport);

// Connect to NVMe controller
// If device_path is provided in transport config, use existing device
// Otherwise, connect to the target using transport info
int nvme_wrapper_connect(struct nvme_context *ctx,
                        struct xcopy_transport_config *transport);

// Connect using existing device path (e.g., /dev/nvme1n1 -> /dev/nvme1)
int nvme_wrapper_connect_device(struct nvme_context *ctx,
                                const char *device_path);

// Get controller file descriptor
int nvme_wrapper_get_ctrl_fd(struct nvme_context *ctx);

// Get namespace file descriptor by ID
int nvme_wrapper_get_ns_fd(struct nvme_context *ctx, uint32_t nsid);

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
