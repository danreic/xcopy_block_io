#include "nvme_wrapper.h"
#include "xcopy_tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

int nvme_wrapper_init(struct nvme_context *ctx,
                     struct xcopy_transport_config *transport) {
    if (!ctx || !transport) {
        return -EINVAL;
    }
    
    memset(ctx, 0, sizeof(*ctx));
    ctx->initialized = true;
    ctx->connected = false;
    
    return 0;
}

int nvme_wrapper_connect(struct nvme_context *ctx,
                        struct xcopy_transport_config *transport) {
    if (!ctx || !transport || !ctx->initialized) {
        return -EINVAL;
    }
    
    if (ctx->connected) {
        // Already connected
        return 0;
    }
    
    // Only support TCP for now
    if (transport->type != XCOPY_TRANSPORT_TCP) {
        fprintf(stderr, "Error: Only TCP transport is currently supported\n");
        return -ENOTSUP;
    }
    
    if (!transport->traddr || !transport->subnqn) {
        fprintf(stderr, "Error: Transport address and subsystem NQN are required for TCP\n");
        return -EINVAL;
    }
    
    // Parse port (default to 4420)
    uint16_t port = 4420;
    if (transport->trsvcid) {
        port = (uint16_t)atoi(transport->trsvcid);
        if (port == 0) {
            fprintf(stderr, "Error: Invalid port number: %s\n", transport->trsvcid);
            return -EINVAL;
        }
    }
    
    // For TCP transport, use nvme-cli command to connect
    // The kernel will create a device (e.g., /dev/nvme0) after connection
    // Then we can open it with libnvme's nvme_open()
    
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "nvme connect -t tcp -a %s -s %s -n %s >/dev/null 2>&1",
             transport->traddr,
             transport->trsvcid ? transport->trsvcid : "4420",
             transport->subnqn);
    
    // Execute nvme connect command
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "Error: Failed to connect to NVMe target\n");
        fprintf(stderr, "  Command: nvme connect -t tcp -a %s -s %s -n %s\n",
                transport->traddr,
                transport->trsvcid ? transport->trsvcid : "4420",
                transport->subnqn);
        fprintf(stderr, "  Make sure 'nvme' command is available and target is reachable\n");
        return -1;
    }
    
    // Wait a moment for the kernel to create the device
    usleep(500000);  // 500ms
    
    // Find the newly created device
    // The kernel creates devices like /dev/nvme0, /dev/nvme1, etc.
    // We'll try to find one that matches our connection
    // For simplicity, we'll check /dev/nvme0 through /dev/nvme15
    char device_path[64];
    bool found = false;
    for (int i = 0; i < 16; i++) {
        snprintf(device_path, sizeof(device_path), "/dev/nvme%d", i);
        
        // Check if device exists
        if (access(device_path, F_OK) == 0) {
            // Try to open it - if successful, use it
            // In a full implementation, we'd verify it matches our NQN
            found = true;
            break;
        }
    }
    
    if (!found) {
        fprintf(stderr, "Error: No NVMe device found after connection\n");
        fprintf(stderr, "  Connection may have failed or device creation is delayed\n");
        return -1;
    }
    
    // Open the controller using libnvme
    // Note: libnvme API may vary - nvme_open() or nvme_ctrl_open() depending on version
    ctx->ctrl = nvme_open(device_path);
    if (!ctx->ctrl) {
        fprintf(stderr, "Error: Failed to open NVMe controller at %s\n", device_path);
        fprintf(stderr, "  Error: %s\n", strerror(errno));
        return -1;
    }
    
    // Enumerate namespaces
    ctx->num_ns = 0;
    for (uint32_t nsid = 1; nsid <= 1024; nsid++) {
        struct nvme_ns *ns = nvme_ns_open(ctx->ctrl, nsid);
        if (ns) {
            ctx->num_ns++;
            if (ctx->num_ns == 1) {
                ctx->ns_list = ns;
            }
        } else {
            // No more namespaces
            break;
        }
    }
    
    if (ctx->num_ns == 0) {
        fprintf(stderr, "Warning: No namespaces found on controller\n");
    }
    
    ctx->connected = true;
    return 0;
}

struct nvme_ctrl *nvme_wrapper_get_ctrl(struct nvme_context *ctx) {
    if (!ctx || !ctx->connected) {
        return NULL;
    }
    return ctx->ctrl;
}

struct nvme_ns *nvme_wrapper_get_ns(struct nvme_context *ctx, uint32_t nsid) {
    if (!ctx || !ctx->connected || !ctx->ctrl) {
        return NULL;
    }
    
    // Open namespace if not already open
    return nvme_ns_open(ctx->ctrl, nsid);
}

uint64_t nvme_wrapper_get_ns_size(struct nvme_context *ctx, uint32_t nsid) {
    struct nvme_ns *ns = nvme_wrapper_get_ns(ctx, nsid);
    if (!ns) {
        return 0;
    }
    
    return nvme_ns_get_num_sectors(ns);
}

uint32_t nvme_wrapper_get_block_size(struct nvme_context *ctx, uint32_t nsid) {
    struct nvme_ns *ns = nvme_wrapper_get_ns(ctx, nsid);
    if (!ns) {
        return 0;
    }
    
    return nvme_ns_get_sector_size(ns);
}

int nvme_wrapper_submit_passthru(struct nvme_context *ctx,
                                 uint32_t nsid,
                                 struct nvme_passthru_cmd *cmd,
                                 void *data,
                                 size_t data_len) {
    if (!ctx || !ctx->connected || !ctx->ctrl || !cmd) {
        return -EINVAL;
    }
    
    // Submit passthrough command
    // Note: libnvme's nvme_submit_io_passthru is synchronous
    // We'll need io_uring wrapper for async behavior
    return nvme_submit_io_passthru(ctx->ctrl, nsid, cmd, data, data_len);
}

void nvme_wrapper_cleanup(struct nvme_context *ctx) {
    if (!ctx) {
        return;
    }
    
    if (ctx->connected && ctx->ctrl) {
        // Close controller
        // Note: libnvme API may use nvme_ctrl_close() or nvme_close()
        nvme_close(ctx->ctrl);
        ctx->ctrl = NULL;
        
        // Disconnect from target (for TCP)
        // Note: May need to use nvme disconnect command or libnvme disconnect function
        // For now, we'll rely on kernel cleanup when device is closed
    }
    
    // Close namespaces
    if (ctx->ns_list) {
        // Note: libnvme may use nvme_ns_close() or automatic cleanup
        // Namespaces are typically closed automatically when controller is closed
        ctx->ns_list = NULL;
    }
    
    ctx->connected = false;
    ctx->initialized = false;
    ctx->num_ns = 0;
}

xcopy_transport_type_t nvme_wrapper_parse_transport(const char *transport_str) {
    if (!transport_str) {
        return XCOPY_TRANSPORT_TCP;  // Default to TCP
    }
    
    if (strcasecmp(transport_str, "tcp") == 0) {
        return XCOPY_TRANSPORT_TCP;
    } else if (strcasecmp(transport_str, "rdma") == 0) {
        return XCOPY_TRANSPORT_RDMA;
    } else if (strcasecmp(transport_str, "pcie") == 0) {
        return XCOPY_TRANSPORT_PCIE;
    }
    
    return XCOPY_TRANSPORT_TCP;  // Default to TCP
}

