#include "spdk_wrapper.h"
#include "xcopy_tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>

// Probe callback for controller attachment
static bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
                     struct spdk_nvme_ctrlr_opts *opts) {
    struct spdk_context *ctx = (struct spdk_context *)cb_ctx;
    
    // Copy transport ID for later use
    memcpy(&ctx->trid, trid, sizeof(*trid));
    
    // Set controller options size explicitly (required by some SPDK versions)
    if (opts) {
        opts->opts_size = sizeof(*opts);
    }
    
    return true;
}

// Attach callback
static void attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
                      struct spdk_nvme_ctrlr *ctrlr,
                      const struct spdk_nvme_ctrlr_opts *opts) {
    (void)trid;  // Unused parameter
    (void)opts;  // Unused parameter
    struct spdk_context *ctx = (struct spdk_context *)cb_ctx;
    ctx->ctrlr = ctrlr;
}

int spdk_wrapper_init(struct spdk_context *ctx,
                     struct xcopy_transport_config *transport) {
    if (!ctx || !transport) {
        return -EINVAL;
    }
    
    memset(ctx, 0, sizeof(*ctx));
    
    // Initialize SPDK environment options
    spdk_env_opts_init(&ctx->opts);
    ctx->opts.name = "xcopy_tool";
    
    // Set opts_size explicitly (required by some SPDK versions)
    ctx->opts.opts_size = sizeof(ctx->opts);
    
    // Configure hugepages - use 2MB pages and allow automatic allocation
    // If hugepages are not available, SPDK will fall back to regular pages
    ctx->opts.hugepage_single_segments = false;
    ctx->opts.unlink_hugepage = false;
    
    // Set memory pool size (in MB) - adjust based on your needs
    // For XCOPY operations, we don't need huge amounts of memory
    ctx->opts.mem_size = 512;  // 512 MB should be sufficient
    
    // Initialize SPDK environment
    if (spdk_env_init(&ctx->opts) < 0) {
        fprintf(stderr, "Failed to initialize SPDK environment\n");
        return -1;
    }
    
    ctx->initialized = true;
    return 0;
}

int spdk_wrapper_probe_attach(struct spdk_context *ctx,
                             struct xcopy_transport_config *transport) {
    if (!ctx || !transport || !ctx->initialized) {
        return -EINVAL;
    }
    
    // Initialize transport ID
    memset(&ctx->trid, 0, sizeof(ctx->trid));
    
    // Set transport type
    switch (transport->type) {
        case XCOPY_TRANSPORT_PCIE:
            ctx->trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
            if (transport->traddr) {
                snprintf(ctx->trid.traddr, sizeof(ctx->trid.traddr), "%s", transport->traddr);
            }
            break;
            
        case XCOPY_TRANSPORT_TCP:
            ctx->trid.trtype = SPDK_NVME_TRANSPORT_TCP;
            if (transport->traddr) {
                snprintf(ctx->trid.traddr, sizeof(ctx->trid.traddr), "%s", transport->traddr);
            }
            if (transport->trsvcid) {
                snprintf(ctx->trid.trsvcid, sizeof(ctx->trid.trsvcid), "%s", transport->trsvcid);
            } else {
                snprintf(ctx->trid.trsvcid, sizeof(ctx->trid.trsvcid), "4420");
            }
            if (transport->subnqn) {
                snprintf(ctx->trid.subnqn, sizeof(ctx->trid.subnqn), "%s", transport->subnqn);
            }
            // hostnqn may not be available in all SPDK versions
            // It's optional and can be set via environment variable SPDK_NVME_HOSTNQN instead
            // if (transport->hostnqn) {
            //     snprintf(ctx->trid.hostnqn, sizeof(ctx->trid.hostnqn), "%s", transport->hostnqn);
            // }
            break;
            
        case XCOPY_TRANSPORT_RDMA:
            ctx->trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
            if (transport->traddr) {
                snprintf(ctx->trid.traddr, sizeof(ctx->trid.traddr), "%s", transport->traddr);
            }
            if (transport->trsvcid) {
                snprintf(ctx->trid.trsvcid, sizeof(ctx->trid.trsvcid), "%s", transport->trsvcid);
            }
            if (transport->subnqn) {
                snprintf(ctx->trid.subnqn, sizeof(ctx->trid.subnqn), "%s", transport->subnqn);
            }
            break;
            
        default:
            fprintf(stderr, "Unsupported transport type\n");
            return -EINVAL;
    }
    
    // Probe for controllers
    if (spdk_nvme_probe(&ctx->trid, ctx, probe_cb, attach_cb, NULL) != 0) {
        fprintf(stderr, "Failed to probe for NVMe controllers\n");
        return -1;
    }
    
    if (!ctx->ctrlr) {
        fprintf(stderr, "No NVMe controller found\n");
        return -1;
    }
    
    return 0;
}

struct spdk_nvme_ctrlr *spdk_wrapper_get_ctrlr(struct spdk_context *ctx) {
    if (!ctx) {
        return NULL;
    }
    return ctx->ctrlr;
}

struct spdk_nvme_ns *spdk_wrapper_get_ns(struct spdk_context *ctx, uint32_t nsid) {
    if (!ctx || !ctx->ctrlr) {
        return NULL;
    }
    return spdk_nvme_ctrlr_get_ns(ctx->ctrlr, nsid);
}

uint64_t spdk_wrapper_get_ns_size(struct spdk_context *ctx, uint32_t nsid) {
    struct spdk_nvme_ns *ns = spdk_wrapper_get_ns(ctx, nsid);
    if (!ns) {
        return 0;
    }
    return spdk_nvme_ns_get_num_sectors(ns);
}

uint32_t spdk_wrapper_get_block_size(struct spdk_context *ctx, uint32_t nsid) {
    struct spdk_nvme_ns *ns = spdk_wrapper_get_ns(ctx, nsid);
    if (!ns) {
        return 0;
    }
    return spdk_nvme_ns_get_sector_size(ns);
}

void spdk_wrapper_cleanup(struct spdk_context *ctx) {
    if (!ctx) {
        return;
    }
    
    if (ctx->ctrlr) {
        spdk_nvme_detach(ctx->ctrlr);
        ctx->ctrlr = NULL;
    }
    
    if (ctx->initialized) {
        spdk_env_fini();
        ctx->initialized = false;
    }
}

xcopy_transport_type_t spdk_wrapper_parse_transport(const char *transport_str) {
    if (!transport_str) {
        return XCOPY_TRANSPORT_PCIE;
    }
    
    if (strcasecmp(transport_str, "tcp") == 0) {
        return XCOPY_TRANSPORT_TCP;
    } else if (strcasecmp(transport_str, "rdma") == 0) {
        return XCOPY_TRANSPORT_RDMA;
    } else if (strcasecmp(transport_str, "pcie") == 0) {
        return XCOPY_TRANSPORT_PCIE;
    }
    
    return XCOPY_TRANSPORT_PCIE;
}

