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
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>
#include <endian.h>

int nvme_wrapper_init(struct nvme_context *ctx,
                     struct xcopy_transport_config *transport) {
    if (!ctx || !transport) {
        return -EINVAL;
    }
    
    memset(ctx, 0, sizeof(*ctx));
    ctx->ctrl_fd = -1;
    ctx->initialized = true;
    ctx->connected = false;
    ctx->ns_capacity = 16;  // Initial capacity
    
    // Allocate namespace list
    ctx->ns_list = calloc(ctx->ns_capacity, sizeof(struct nvme_ns_info));
    if (!ctx->ns_list) {
        return -ENOMEM;
    }
    
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
    
    // For TCP transport, use nvme-cli command to connect
    // The kernel will create a device (e.g., /dev/nvme0) after connection
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
    char device_path[64];
    bool found = false;
    for (int i = 0; i < 16; i++) {
        snprintf(device_path, sizeof(device_path), "/dev/nvme%d", i);
        
        // Check if device exists
        if (access(device_path, F_OK) == 0) {
            found = true;
            break;
        }
    }
    
    if (!found) {
        fprintf(stderr, "Error: No NVMe device found after connection\n");
        fprintf(stderr, "  Connection may have failed or device creation is delayed\n");
        return -1;
    }
    
    // Open the controller device (e.g., /dev/nvme0)
    ctx->ctrl_fd = open(device_path, O_RDWR);
    if (ctx->ctrl_fd < 0) {
        fprintf(stderr, "Error: Failed to open NVMe controller at %s: %s\n", 
                device_path, strerror(errno));
        return -1;
    }
    
    strncpy(ctx->device_path, device_path, sizeof(ctx->device_path) - 1);
    ctx->device_path[sizeof(ctx->device_path) - 1] = '\0';
    
    // Enumerate namespaces by trying to open each namespace device
    // Namespace devices are like /dev/nvme0n1, /dev/nvme0n2, etc.
    ctx->num_ns = 0;
    for (uint32_t nsid = 1; nsid <= 256; nsid++) {
        char ns_path[64];
        snprintf(ns_path, sizeof(ns_path), "%sn%d", device_path, nsid);
        
        int ns_fd = open(ns_path, O_RDONLY);
        if (ns_fd < 0) {
            // No more namespaces
            break;
        }
        
        // Get namespace size using ioctl
        // Use nvme_passthru_cmd for admin commands
        struct nvme_id_ns ns_id;
        struct nvme_passthru_cmd admin_cmd = {
            .opcode = 0x06,  // NVME_ADMIN_IDENTIFY
            .flags = 0,
            .rsvd1 = 0,
            .nsid = nsid,
            .cdw2 = 0,
            .cdw3 = 0,
            .metadata = 0,
            .addr = (__u64)(uintptr_t)&ns_id,
            .metadata_len = 0,
            .data_len = sizeof(ns_id),
            .cdw10 = 0,  // CNS = 0 (identify namespace)
            .cdw11 = 0,
            .cdw12 = 0,
            .cdw13 = 0,
            .cdw14 = 0,
            .cdw15 = 0,
            .timeout_ms = 0,
            .result = 0,
        };
        
        if (ioctl(ctx->ctrl_fd, NVME_IOCTL_ADMIN_CMD, &admin_cmd) == 0) {
            // Resize namespace list if needed
            if (ctx->num_ns >= ctx->ns_capacity) {
                uint32_t new_capacity = ctx->ns_capacity * 2;
                struct nvme_ns_info *new_list = realloc(ctx->ns_list, 
                                                        new_capacity * sizeof(struct nvme_ns_info));
                if (!new_list) {
                    close(ns_fd);
                    break;
                }
                ctx->ns_list = new_list;
                ctx->ns_capacity = new_capacity;
            }
            
            // Calculate namespace size
            // ns_id.nsze is in little-endian format from kernel
            // Use memcpy to avoid alignment issues, then convert
            uint64_t nsze_le;
            memcpy(&nsze_le, &ns_id.nsze, sizeof(nsze_le));
            uint64_t nsze = le64toh(nsze_le);
            uint32_t lbaf = ns_id.flbas & 0xf;
            uint32_t lba_size = 1 << ns_id.lbaf[lbaf].ds;
            
            ctx->ns_list[ctx->num_ns].nsid = nsid;
            ctx->ns_list[ctx->num_ns].size_blocks = nsze;
            ctx->ns_list[ctx->num_ns].block_size = lba_size;
            ctx->ns_list[ctx->num_ns].fd = ns_fd;
            ctx->num_ns++;
        } else {
            close(ns_fd);
        }
    }
    
    if (ctx->num_ns == 0) {
        fprintf(stderr, "Warning: No namespaces found on controller\n");
    }
    
    ctx->connected = true;
    return 0;
}

int nvme_wrapper_get_ctrl_fd(struct nvme_context *ctx) {
    if (!ctx || !ctx->connected) {
        return -1;
    }
    return ctx->ctrl_fd;
}

int nvme_wrapper_get_ns_fd(struct nvme_context *ctx, uint32_t nsid) {
    if (!ctx || !ctx->connected) {
        return -1;
    }
    
    for (uint32_t i = 0; i < ctx->num_ns; i++) {
        if (ctx->ns_list[i].nsid == nsid) {
            return ctx->ns_list[i].fd;
        }
    }
    
    return -1;
}

uint64_t nvme_wrapper_get_ns_size(struct nvme_context *ctx, uint32_t nsid) {
    if (!ctx || !ctx->connected) {
        return 0;
    }
    
    for (uint32_t i = 0; i < ctx->num_ns; i++) {
        if (ctx->ns_list[i].nsid == nsid) {
            return ctx->ns_list[i].size_blocks;
        }
    }
    
    return 0;
}

uint32_t nvme_wrapper_get_block_size(struct nvme_context *ctx, uint32_t nsid) {
    if (!ctx || !ctx->connected) {
        return 0;
    }
    
    for (uint32_t i = 0; i < ctx->num_ns; i++) {
        if (ctx->ns_list[i].nsid == nsid) {
            return ctx->ns_list[i].block_size;
        }
    }
    
    return 0;
}

int nvme_wrapper_submit_passthru(struct nvme_context *ctx,
                                 uint32_t nsid,
                                 struct nvme_passthru_cmd *cmd,
                                 void *data,
                                 size_t data_len) {
    if (!ctx || !ctx->connected || ctx->ctrl_fd < 0 || !cmd) {
        return -EINVAL;
    }
    
    // Set namespace ID in command
    cmd->nsid = nsid;
    
    // Prepare ioctl structure
    struct nvme_passthru_cmd ioctl_cmd = *cmd;
    
    // Set data pointer if provided
    if (data && data_len > 0) {
        ioctl_cmd.addr = (__u64)(uintptr_t)data;
        ioctl_cmd.data_len = data_len;
    }
    
    // XCOPY is an I/O command (opcode 0x19), use IO_CMD
    __u32 result = 0;
    int ret = ioctl(ctx->ctrl_fd, NVME_IOCTL_IO_CMD, &ioctl_cmd);
    if (ret < 0) {
        return -errno;
    }
    
    result = ioctl_cmd.result;
    
    // Check result - result contains status field
    // Status code is in bits 15:1, phase bit is bit 0
    __u16 status = (result >> 1) & 0x7FFF;
    if (status != 0) {
        return -(int)status;
    }
    
    return 0;
}

void nvme_wrapper_cleanup(struct nvme_context *ctx) {
    if (!ctx || !ctx->initialized) {
        return;
    }
    
    // Close namespace file descriptors
    if (ctx->ns_list) {
        for (uint32_t i = 0; i < ctx->num_ns; i++) {
            if (ctx->ns_list[i].fd >= 0) {
                close(ctx->ns_list[i].fd);
            }
        }
        free(ctx->ns_list);
        ctx->ns_list = NULL;
    }
    
    // Close controller file descriptor
    if (ctx->ctrl_fd >= 0) {
        close(ctx->ctrl_fd);
        ctx->ctrl_fd = -1;
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
