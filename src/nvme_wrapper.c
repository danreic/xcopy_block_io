#include "nvme_wrapper.h"
#include "xcopy_tool.h"
#include "xcopy_cmd.h"
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
// libnvme.h is already included via nvme_wrapper.h

int nvme_wrapper_init(struct nvme_context *ctx,
                     struct xcopy_transport_config *transport) {
    (void)transport;  // Unused parameter - kept for API compatibility
    
    if (!ctx) {
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

// Helper function to discover namespaces using libnvme
static int discover_namespaces(struct nvme_context *ctx, int ctrl_fd) {
    if (!ctx || ctrl_fd < 0) {
        return -EINVAL;
    }
    
    // Clear existing namespace list
    ctx->num_ns = 0;
    
    // Try to discover namespaces by attempting to identify each one
    // Start from NSID 1 (NSID 0 is invalid)
    for (uint32_t nsid = 1; nsid <= 256; nsid++) {
        struct nvme_id_ns ns_id;
        int err;
        
        // Use libnvme's nvme_identify_ns() helper (takes fd, not handle)
        err = nvme_identify_ns(ctrl_fd, nsid, &ns_id);
        if (err != 0) {
            // No more namespaces
            break;
        }
        
        // Resize namespace list if needed
        if (ctx->num_ns >= ctx->ns_capacity) {
            uint32_t new_capacity = ctx->ns_capacity * 2;
            struct nvme_ns_info *new_list = realloc(ctx->ns_list, 
                                                    new_capacity * sizeof(struct nvme_ns_info));
            if (!new_list) {
                break;
            }
            ctx->ns_list = new_list;
            ctx->ns_capacity = new_capacity;
        }
        
        // Extract namespace size and block size
        uint64_t nsze_le;
        memcpy(&nsze_le, &ns_id.nsze, sizeof(nsze_le));
        uint64_t nsze = le64toh(nsze_le);
        uint32_t lbaf = ns_id.flbas & 0xf;
        uint32_t lba_size = 1 << ns_id.lbaf[lbaf].ds;
        
        // Try to open namespace device
        char ns_path[64];
        snprintf(ns_path, sizeof(ns_path), "%.*sn%u", 
                 (int)(sizeof(ns_path) - 10), ctx->device_path, nsid);
        int ns_fd = open(ns_path, O_RDONLY);
        
        ctx->ns_list[ctx->num_ns].nsid = nsid;
        ctx->ns_list[ctx->num_ns].size_blocks = nsze;
        ctx->ns_list[ctx->num_ns].block_size = lba_size;
        ctx->ns_list[ctx->num_ns].fd = ns_fd;  // May be -1 if open failed
        ctx->num_ns++;
    }
    
    if (ctx->num_ns == 0) {
        fprintf(stderr, "Warning: No namespaces found on controller\n");
    }
    
    return 0;
}

// Helper function to extract controller path from namespace path
// /dev/nvme1n1 -> /dev/nvme1
static int extract_controller_path(const char *ns_path, char *ctrl_path, size_t len) {
    if (!ns_path || !ctrl_path || len == 0) {
        return -1;
    }
    
    const char *basename = strrchr(ns_path, '/');
    if (!basename) {
        basename = ns_path;
    } else {
        basename++;
    }
    
    // Check format: nvmeXnY where X is controller, Y is namespace
    if (strncmp(basename, "nvme", 4) != 0) {
        return -1;
    }
    
    const char *p = basename + 4;
    const char *n_pos = strchr(p, 'n');
    if (!n_pos) {
        return -1;
    }
    
    size_t ctrl_len = n_pos - basename;
    const char *dir = strrchr(ns_path, '/');
    if (dir) {
        size_t dir_len = dir - ns_path + 1;
        if (snprintf(ctrl_path, len, "%.*s%.*s", 
                     (int)dir_len, ns_path, (int)ctrl_len, basename) >= (int)len) {
            return -1;
        }
    } else {
        if (snprintf(ctrl_path, len, "%.*s", (int)ctrl_len, basename) >= (int)len) {
            return -1;
        }
    }
    
    return 0;
}

int nvme_wrapper_connect_device(struct nvme_context *ctx,
                                const char *device_path) {
    if (!ctx || !device_path || !ctx->initialized) {
        return -EINVAL;
    }
    
    if (ctx->connected) {
        return 0;
    }
    
    // Extract controller path from namespace path
    char ctrl_path[64];
    if (extract_controller_path(device_path, ctrl_path, sizeof(ctrl_path)) != 0) {
        fprintf(stderr, "Error: Invalid device path format: %s\n", device_path);
        return -1;
    }
    
    // Check if controller device exists
    if (access(ctrl_path, F_OK) != 0) {
        fprintf(stderr, "Error: Controller device %s does not exist\n", ctrl_path);
        return -1;
    }
    
    // Use regular open() for controller device (nvme_open() may require namespace device)
    ctx->ctrl_fd = open(ctrl_path, O_RDWR);
    if (ctx->ctrl_fd < 0) {
        fprintf(stderr, "Error: Failed to open NVMe controller at %s: %s\n", 
                ctrl_path, strerror(errno));
        return -1;
    }
    
    strncpy(ctx->device_path, ctrl_path, sizeof(ctx->device_path) - 1);
    ctx->device_path[sizeof(ctx->device_path) - 1] = '\0';
    
    // Discover namespaces using libnvme
    if (discover_namespaces(ctx, ctx->ctrl_fd) != 0) {
        close(ctx->ctrl_fd);
        ctx->ctrl_fd = -1;
        return -1;
    }
    
    ctx->connected = true;
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
        snprintf(device_path, sizeof(device_path), "/dev/nvme%dn1", i);  // Try namespace device first
        
        // Check if namespace device exists
        if (access(device_path, F_OK) == 0) {
            found = true;
            break;
        }
        
        // Also try controller device
        snprintf(device_path, sizeof(device_path), "/dev/nvme%d", i);
        if (access(device_path, F_OK) == 0) {
            // Try to find first namespace
            for (uint32_t nsid = 1; nsid <= 16; nsid++) {
                char ns_path[64];
                snprintf(ns_path, sizeof(ns_path), "/dev/nvme%dn%u", i, nsid);
                if (access(ns_path, F_OK) == 0) {
                    snprintf(device_path, sizeof(device_path), "/dev/nvme%dn%u", i, nsid);
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
    }
    
    if (!found) {
        fprintf(stderr, "Error: No NVMe device found after connection\n");
        fprintf(stderr, "  Connection may have failed or device creation is delayed\n");
        return -1;
    }
    
    // Extract controller path from namespace path if needed
    char ctrl_path[64];
    if (extract_controller_path(device_path, ctrl_path, sizeof(ctrl_path)) != 0) {
        // If extraction fails, assume it's already a controller path
        strncpy(ctrl_path, device_path, sizeof(ctrl_path) - 1);
        ctrl_path[sizeof(ctrl_path) - 1] = '\0';
    }
    
    // Check if controller device exists
    if (access(ctrl_path, F_OK) != 0) {
        fprintf(stderr, "Error: Controller device %s does not exist\n", ctrl_path);
        return -1;
    }
    
    // Use regular open() for controller device (nvme_open() may require namespace device)
    ctx->ctrl_fd = open(ctrl_path, O_RDWR);
    if (ctx->ctrl_fd < 0) {
        fprintf(stderr, "Error: Failed to open NVMe controller at %s: %s\n", 
                ctrl_path, strerror(errno));
        return -1;
    }
    
    strncpy(ctx->device_path, ctrl_path, sizeof(ctx->device_path) - 1);
    ctx->device_path[sizeof(ctx->device_path) - 1] = '\0';
    
    // Discover namespaces using libnvme
    if (discover_namespaces(ctx, ctx->ctrl_fd) != 0) {
        close(ctx->ctrl_fd);
        ctx->ctrl_fd = -1;
        return -1;
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
    // Validate context thoroughly
    if (!ctx) {
        fprintf(stderr, "nvme_wrapper_submit_passthru: ctx is NULL\n");
        return -EINVAL;
    }
    
    if (!ctx->initialized) {
        fprintf(stderr, "nvme_wrapper_submit_passthru: ctx not initialized\n");
        return -EINVAL;
    }
    
    if (!ctx->connected || ctx->ctrl_fd < 0) {
        static int not_connected_logged = 0;
        if (!not_connected_logged) {
            fprintf(stderr, "nvme_wrapper_submit_passthru: ctx not connected (connected=%d, ctrl_fd=%d, initialized=%d)\n", 
                    ctx->connected, ctx->ctrl_fd, ctx->initialized);
            not_connected_logged = 1;
        }
        return -EINVAL;
    }
    
    if (!cmd) {
        fprintf(stderr, "nvme_wrapper_submit_passthru: cmd is NULL\n");
        return -EINVAL;
    }
    
    // Set namespace ID in command
    cmd->nsid = nsid;
    
    // Set data pointer and length if provided
    // libnvme handles page alignment internally
    if (data && data_len > 0) {
        cmd->addr = (__u64)(uintptr_t)data;
        cmd->data_len = data_len;
        
        // Debug: Print detailed information about data buffer (first time for XCOPY)
        // Note: The buffer contains struct nvme_copy_range_f2, not struct copy_range_descriptor
        static int range_data_debug_logged = 0;
        if (!range_data_debug_logged && data_len >= sizeof(struct nvme_copy_range_f2)) {
            struct nvme_copy_range_f2 *first_range = (struct nvme_copy_range_f2 *)data;
            uint32_t src_nsid = le32toh(first_range->snsid);
            uint64_t src_lba = le64toh(first_range->slba);
            uint16_t num_blocks = le16toh(first_range->nlb);
            fprintf(stderr, "DEBUG: Range descriptor in data buffer (format 2): src_nsid=0x%x (%u), src_lba=0x%lx (%lu), num_blocks=0x%x (%u)\n",
                    src_nsid, src_nsid, src_lba, src_lba, num_blocks, num_blocks);
            
            // Hex dump of first 64 bytes
            fprintf(stderr, "DEBUG: Hex dump of buffer being sent (first 64 bytes):\n");
            uint8_t *buffer_bytes = (uint8_t *)data;
            for (int i = 0; i < 64 && i < (int)data_len; i += 16) {
                fprintf(stderr, "  %04x: ", i);
                for (int j = 0; j < 16 && (i + j) < (int)data_len; j++) {
                    fprintf(stderr, "%02x ", buffer_bytes[i + j]);
                }
                fprintf(stderr, "\n");
            }
            range_data_debug_logged = 1;
        }
    } else {
        // XCOPY commands MUST have data (range descriptors)
        if (cmd->opcode == NVME_OPC_COPY) {
            static int missing_data_warned = 0;
            if (!missing_data_warned) {
                fprintf(stderr, "WARNING: XCOPY command has no data buffer! This is invalid - XCOPY requires range descriptors.\n");
                missing_data_warned = 1;
            }
        }
        cmd->addr = 0;
        cmd->data_len = 0;
    }
    
    // For XCOPY commands, try namespace FD first (like nvme-cli does for I/O commands)
    // If namespace FD is not available, fall back to controller FD
    // XCOPY is an I/O command, so it should use namespace FD
    int ns_fd = nvme_wrapper_get_ns_fd(ctx, nsid);
    int target_fd = (ns_fd >= 0) ? ns_fd : ctx->ctrl_fd;
    
    // Debug: Log which FD we're using
    static int fd_choice_logged = 0;
    if (!fd_choice_logged) {
        fprintf(stderr, "DEBUG: Using %s for XCOPY command (ns_fd=%d, ctrl_fd=%d)\n",
                (ns_fd >= 0) ? "namespace FD" : "controller FD", ns_fd, ctx->ctrl_fd);
        fd_choice_logged = 1;
    }
    
    // Use libnvme's nvme_submit_io_passthru() (takes fd, not handle)
    // This handles all the low-level details including proper command formatting
    __u32 result = 0;
    int err = nvme_submit_io_passthru(target_fd, cmd, &result);
    
    // Store result in command structure
    cmd->result = result;
    
    // Debug: Print command details (first time only)
    static int debug_count = 0;
    if (debug_count < 1) {
        fprintf(stderr, "DEBUG: Submitting NVMe command via libnvme: opcode=0x%x, nsid=%u, data_len=%u, fd=%d\n",
                cmd->opcode, cmd->nsid, cmd->data_len, target_fd);
        fprintf(stderr, "DEBUG: Command CDW10=0x%x (num_ranges-1), CDW11=0x%x, CDW12=0x%x\n",
                cmd->cdw10, cmd->cdw11, cmd->cdw12);
        debug_count++;
    }
    
    if (err < 0) {
        // Command submission failed
        static int error_log_count = 0;
        if (error_log_count < 5) {
            fprintf(stderr, "ERROR: NVMe command submission failed: %s (opcode=0x%x, nsid=%u, data_len=%u)\n",
                    strerror(errno), cmd->opcode, nsid, cmd->data_len);
            error_log_count++;
        }
        return err;
    }
    
    // Check command status from result field
    __u16 status = (result >> 1) & 0x7FFF;
    __u8 status_type = (status >> 9) & 0x7;
    __u16 status_code = status & 0xFF;
    
    // Log result (first few times or on errors)
    // Note: nvme_submit_io_passthru may return number of bytes transferred on success
    // Check the status field in result to determine actual command status
    static int result_log_count = 0;
    if (result_log_count < 3 || status != 0 || result != 0 || err != 0) {
        fprintf(stderr, "DEBUG: Command submission: err=%d (0=success, <0=error), result=0x%x, status=0x%x (type=%u, code=0x%x)\n",
                err, result, status, status_type, status_code);
        if (err > 0) {
            fprintf(stderr, "DEBUG: Note: Positive return value (%d) may indicate bytes transferred, check status field\n", err);
        }
        result_log_count++;
    }
    
    if (status != 0) {
        // Log status for debugging
        static int status_log_count = 0;
        if (status_log_count < 10) {
            fprintf(stderr, "ERROR: NVMe command returned non-zero status: 0x%x (result=0x%x, type=%u, code=0x%x, opcode=0x%x, nsid=%u)\n",
                    status, result, status_type, status_code, cmd->opcode, nsid);
            status_log_count++;
        }
        return -(int)status;
    }
    
    // Command succeeded (status == 0)
    static int success_logged = 0;
    if (!success_logged) {
        fprintf(stderr, "DEBUG: NVMe command submitted successfully (err=%d, result=0x%x, status=0x%x)\n",
                err, result, status);
        success_logged = 1;
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
