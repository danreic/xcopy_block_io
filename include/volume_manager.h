#ifndef VOLUME_MANAGER_H
#define VOLUME_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <spdk/nvme.h>

// Volume/Namespace information
struct volume_info {
    uint32_t nsid;
    struct spdk_nvme_ns *ns;
    struct spdk_nvme_ctrlr *ctrlr;
    uint64_t size_blocks;
    uint32_t block_size;
    char name[256];            // Human-readable name
};

// Volume manager context
struct volume_manager {
    struct volume_info *volumes;
    uint32_t num_volumes;
    uint32_t capacity;
};

// Initialize volume manager
int volume_manager_init(struct volume_manager *vm);

// Add a volume/namespace
int volume_manager_add(struct volume_manager *vm,
                      uint32_t nsid,
                      struct spdk_nvme_ns *ns,
                      struct spdk_nvme_ctrlr *ctrlr);

// Get volume by namespace ID
struct volume_info *volume_manager_get(struct volume_manager *vm, uint32_t nsid);

// Get volume by index
struct volume_info *volume_manager_get_by_index(struct volume_manager *vm, uint32_t index);

// Validate namespace IDs for cross-volume operations
bool volume_manager_validate_cross_volume(struct volume_manager *vm,
                                         uint32_t src_nsid,
                                         uint32_t dst_nsid);

// Cleanup volume manager
void volume_manager_cleanup(struct volume_manager *vm);

// Print volume information
void volume_manager_print_volumes(struct volume_manager *vm);

#endif // VOLUME_MANAGER_H

