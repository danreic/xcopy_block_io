#include "volume_manager.h"
#include "nvme_wrapper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define VOLUME_MANAGER_INITIAL_CAPACITY 16

int volume_manager_init(struct volume_manager *vm) {
    if (!vm) {
        return -EINVAL;
    }
    
    memset(vm, 0, sizeof(*vm));
    
    // Allocate initial volume array
    vm->capacity = VOLUME_MANAGER_INITIAL_CAPACITY;
    vm->volumes = calloc(vm->capacity, sizeof(struct volume_info));
    if (!vm->volumes) {
        return -ENOMEM;
    }
    
    vm->num_volumes = 0;
    return 0;
}

int volume_manager_add(struct volume_manager *vm,
                      uint32_t nsid,
                      int ns_fd,
                      int ctrl_fd) {
    if (!vm || ns_fd < 0 || ctrl_fd < 0) {
        return -EINVAL;
    }
    
    // Check if volume already exists
    for (uint32_t i = 0; i < vm->num_volumes; i++) {
        if (vm->volumes[i].nsid == nsid) {
            // Update existing volume
            vm->volumes[i].ns_fd = ns_fd;
            vm->volumes[i].ctrl_fd = ctrl_fd;
            // Size and block size should be set by caller
            snprintf(vm->volumes[i].name, sizeof(vm->volumes[i].name),
                    "NSID %u", nsid);
            return 0;
        }
    }
    
    // Resize array if needed
    if (vm->num_volumes >= vm->capacity) {
        uint32_t new_capacity = vm->capacity * 2;
        struct volume_info *new_volumes = realloc(vm->volumes,
                                                  new_capacity * sizeof(struct volume_info));
        if (!new_volumes) {
            return -ENOMEM;
        }
        vm->volumes = new_volumes;
        vm->capacity = new_capacity;
    }
    
    // Add new volume
    struct volume_info *vol = &vm->volumes[vm->num_volumes];
    vol->nsid = nsid;
    vol->ns_fd = ns_fd;
    vol->ctrl_fd = ctrl_fd;
    // Size and block size should be set by caller
    snprintf(vol->name, sizeof(vol->name), "NSID %u", nsid);
    
    vm->num_volumes++;
    return 0;
}

struct volume_info *volume_manager_get(struct volume_manager *vm, uint32_t nsid) {
    if (!vm) {
        return NULL;
    }
    
    for (uint32_t i = 0; i < vm->num_volumes; i++) {
        if (vm->volumes[i].nsid == nsid) {
            return &vm->volumes[i];
        }
    }
    
    return NULL;
}

struct volume_info *volume_manager_get_by_index(struct volume_manager *vm, uint32_t index) {
    if (!vm || index >= vm->num_volumes) {
        return NULL;
    }
    
    return &vm->volumes[index];
}

bool volume_manager_validate_cross_volume(struct volume_manager *vm,
                                         uint32_t src_nsid,
                                         uint32_t dst_nsid) {
    if (!vm) {
        return false;
    }
    
    struct volume_info *src_vol = volume_manager_get(vm, src_nsid);
    struct volume_info *dst_vol = volume_manager_get(vm, dst_nsid);
    
    if (!src_vol || !dst_vol) {
        return false;
    }
    
    // Both volumes must exist
    // Additional validation can be added here (e.g., block size compatibility)
    if (src_vol->block_size != dst_vol->block_size) {
        return false;
    }
    
    return true;
}

void volume_manager_cleanup(struct volume_manager *vm) {
    if (!vm) {
        return;
    }
    
    if (vm->volumes) {
        free(vm->volumes);
        vm->volumes = NULL;
    }
    
    vm->num_volumes = 0;
    vm->capacity = 0;
}

void volume_manager_print_volumes(struct volume_manager *vm) {
    if (!vm) {
        return;
    }
    
    printf("Available volumes:\n");
    for (uint32_t i = 0; i < vm->num_volumes; i++) {
        struct volume_info *vol = &vm->volumes[i];
        printf("  [%u] %s: NSID=%u, Size=%lu blocks, Block Size=%u bytes\n",
               i, vol->name, vol->nsid, vol->size_blocks, vol->block_size);
    }
}

