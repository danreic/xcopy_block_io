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
#include <syslog.h>  // For LOG_INFO
// libnvme.h is already included via nvme_wrapper.h

int nvme_wrapper_init(struct nvme_context *ctx,
                     struct xcopy_transport_config *transport) {
    if (!ctx) {
        return -EINVAL;
    }
    
    memset(ctx, 0, sizeof(*ctx));
    ctx->initialized = false;
    ctx->connected = false;
    ctx->ns_capacity = 16;  // Initial capacity
    
    // Create libnvme global context (like nvme-cli does)
    // Use stderr for logging, default log level
    ctx->global_ctx = nvme_create_global_ctx(stderr, LOG_INFO);
    if (!ctx->global_ctx) {
        fprintf(stderr, "Error: Failed to create libnvme global context\n");
        return -ENOMEM;
    }
    
    // Initialize libnvme logging (like nvme-cli does)
    nvme_init_default_logging(stderr, LOG_INFO, false, false);
    
    // Allocate namespace list
    ctx->ns_list = calloc(ctx->ns_capacity, sizeof(struct nvme_ns_info));
    if (!ctx->ns_list) {
        nvme_free_global_ctx(ctx->global_ctx);
        ctx->global_ctx = NULL;
        return -ENOMEM;
    }
    
    ctx->initialized = true;
    return 0;
}

// Helper function to discover namespaces using libnvme
static int discover_namespaces(struct nvme_context *ctx, struct nvme_transport_handle *hdl) {
    if (!ctx || !hdl) {
        return -EINVAL;
    }
    
    // Clear existing namespace list
    ctx->num_ns = 0;
    
    // Try to discover namespaces by attempting to identify each one
    // Start from NSID 1 (NSID 0 is invalid)
    for (uint32_t nsid = 1; nsid <= 256; nsid++) {
        struct nvme_id_ns ns_id;
        int err;
        
        // Try to identify the namespace
        err = nvme_identify_ns(hdl, nsid, &ns_id);
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
        
        ctx->ns_list[ctx->num_ns].nsid = nsid;
        ctx->ns_list[ctx->num_ns].size_blocks = nsze;
        ctx->ns_list[ctx->num_ns].block_size = lba_size;
        ctx->num_ns++;
    }
    
    if (ctx->num_ns == 0) {
        fprintf(stderr, "Warning: No namespaces found on controller\n");
    }
    
    return 0;
}

int nvme_wrapper_connect_device(struct nvme_context *ctx,
                                const char *device_path) {
    if (!ctx || !device_path || !ctx->initialized || !ctx->global_ctx) {
        return -EINVAL;
    }
    
    if (ctx->connected) {
        return 0;
    }
    
    // Use libnvme's nvme_open() to get transport handle (like nvme-cli does)
    struct nvme_transport_handle *hdl = NULL;
    int err = nvme_open(ctx->global_ctx, device_path, &hdl);
    if (err != 0) {
        fprintf(stderr, "Error: Failed to open NVMe device %s: %s\n", 
                device_path, nvme_strerror(err));
        return err;
    }
    
    ctx->hdl = hdl;
    strncpy(ctx->device_path, device_path, sizeof(ctx->device_path) - 1);
    ctx->device_path[sizeof(ctx->device_path) - 1] = '\0';
    
    // Discover namespaces using libnvme
    if (discover_namespaces(ctx, hdl) != 0) {
        nvme_close(hdl);
        ctx->hdl = NULL;
        return -1;
    }
    
    ctx->connected = true;
    return 0;
}

int nvme_wrapper_connect(struct nvme_context *ctx,
                        struct xcopy_transport_config *transport) {
    if (!ctx || !transport || !ctx->initialized || !ctx->global_ctx) {
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
    
    // Use libnvme's nvme_open() to get transport handle (like nvme-cli does)
    struct nvme_transport_handle *hdl = NULL;
    ret = nvme_open(ctx->global_ctx, device_path, &hdl);
    if (ret != 0) {
        fprintf(stderr, "Error: Failed to open NVMe device %s: %s\n", 
                device_path, nvme_strerror(ret));
        return ret;
    }
    
    ctx->hdl = hdl;
    strncpy(ctx->device_path, device_path, sizeof(ctx->device_path) - 1);
    ctx->device_path[sizeof(ctx->device_path) - 1] = '\0';
    
    // Discover namespaces using libnvme
    if (discover_namespaces(ctx, hdl) != 0) {
        nvme_close(hdl);
        ctx->hdl = NULL;
        return -1;
    }
    
    ctx->connected = true;
    return 0;
}

struct nvme_transport_handle *nvme_wrapper_get_handle(struct nvme_context *ctx) {
    if (!ctx || !ctx->connected) {
        return NULL;
    }
    return ctx->hdl;
}

struct nvme_global_ctx *nvme_wrapper_get_global_ctx(struct nvme_context *ctx) {
    if (!ctx || !ctx->initialized) {
        return NULL;
    }
    return ctx->global_ctx;
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
    
    if (!ctx->connected || !ctx->hdl) {
        static int not_connected_logged = 0;
        if (!not_connected_logged) {
            fprintf(stderr, "nvme_wrapper_submit_passthru: ctx not connected (connected=%d, hdl=%p, initialized=%d)\n", 
                    ctx->connected, ctx->hdl, ctx->initialized);
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
        static int range_data_debug_logged = 0;
        if (!range_data_debug_logged && data_len >= sizeof(struct copy_range_descriptor)) {
            struct copy_range_descriptor *first_range = (struct copy_range_descriptor *)data;
            uint32_t src_nsid_le = first_range->src_nsid;
            uint64_t src_lba_le = first_range->src_lba;
            uint32_t num_blocks_le = first_range->num_blocks;
            uint64_t dst_lba_le = first_range->dst_lba;
            fprintf(stderr, "DEBUG: Range descriptor in data buffer (little-endian): src_nsid=0x%x, src_lba=0x%lx, num_blocks=0x%x, dst_lba=0x%lx\n",
                    le32toh(src_nsid_le), le64toh(src_lba_le), le32toh(num_blocks_le), le64toh(dst_lba_le));
            
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
    
    // Use libnvme's nvme_submit_io_passthru() (like nvme-cli does)
    // This handles all the low-level details including proper command formatting
    int err = nvme_submit_io_passthru(ctx->hdl, cmd);
    
    // Debug: Print command details (first time only)
    static int debug_count = 0;
    if (debug_count < 1) {
        fprintf(stderr, "DEBUG: Submitting NVMe command via libnvme: opcode=0x%x, nsid=%u, data_len=%u\n",
                cmd->opcode, cmd->nsid, cmd->data_len);
        fprintf(stderr, "DEBUG: Command CDW10=0x%x (num_ranges-1), CDW11=0x%x, CDW12=0x%x\n",
                cmd->cdw10, cmd->cdw11, cmd->cdw12);
        debug_count++;
    }
    
    if (err < 0) {
        // Command submission failed
        static int error_log_count = 0;
        if (error_log_count < 5) {
            fprintf(stderr, "ERROR: NVMe command submission failed: %s (opcode=0x%x, nsid=%u, data_len=%u)\n",
                    nvme_strerror(err), cmd->opcode, nsid, cmd->data_len);
            error_log_count++;
        }
        return err;
    }
    
    // Check command status from result field
    __u32 result = cmd->result;
    __u16 status = (result >> 1) & 0x7FFF;
    __u8 status_type = (status >> 9) & 0x7;
    __u16 status_code = status & 0xFF;
    
    // Log result (first few times or on errors)
    static int result_log_count = 0;
    if (result_log_count < 3 || status != 0 || result != 0) {
        fprintf(stderr, "DEBUG: Command submission returned %d, command result=0x%x, status=0x%x (type=%u, code=0x%x)\n",
                err, result, status, status_type, status_code);
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
    
    // Close transport handle (like nvme-cli does)
    if (ctx->hdl) {
        nvme_close(ctx->hdl);
        ctx->hdl = NULL;
    }
    
    // Free namespace list
    if (ctx->ns_list) {
        free(ctx->ns_list);
        ctx->ns_list = NULL;
    }
    
    // Free global context (like nvme-cli does)
    if (ctx->global_ctx) {
        nvme_free_global_ctx(ctx->global_ctx);
        ctx->global_ctx = NULL;
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
