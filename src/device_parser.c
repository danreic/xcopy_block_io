#include "device_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

int device_parser_parse_path(const char *device_path, struct device_info *info) {
    if (!device_path || !info) {
        return -1;
    }
    
    memset(info, 0, sizeof(*info));
    strncpy(info->device_path, device_path, sizeof(info->device_path) - 1);
    
    // Parse /dev/nvmeXnY format
    // nvme1n1 -> controller=1, namespace=1
    // nvme2n1 -> controller=2, namespace=1
    // nvme1n2 -> controller=1, namespace=2
    
    const char *basename = strrchr(device_path, '/');
    if (!basename) {
        basename = device_path;
    } else {
        basename++; // Skip '/'
    }
    
    // Check if it matches nvmeXnY pattern
    if (strncmp(basename, "nvme", 4) != 0) {
        return -1;
    }
    
    const char *p = basename + 4; // Skip "nvme"
    
    // Parse controller number
    char *endptr;
    unsigned long ctrl_id = strtoul(p, &endptr, 10);
    if (endptr == p || *endptr != 'n') {
        return -1;
    }
    
    info->controller_id = (uint32_t)ctrl_id;
    p = endptr + 1; // Skip 'n'
    
    // Parse namespace number
    unsigned long ns_id = strtoul(p, &endptr, 10);
    if (endptr == p || *endptr != '\0') {
        return -1;
    }
    
    info->namespace_id = (uint32_t)ns_id;
    info->valid = true;
    
    return 0;
}

int device_parser_get_controller_path(const char *device_path, char *controller_path, size_t len) {
    if (!device_path || !controller_path || len == 0) {
        return -1;
    }
    
    struct device_info info;
    if (device_parser_parse_path(device_path, &info) != 0) {
        return -1;
    }
    
    // Build controller sysfs path: /sys/class/nvme/nvmeX
    int ret = snprintf(controller_path, len, "/sys/class/nvme/nvme%u", info.controller_id);
    if (ret < 0 || (size_t)ret >= len) {
        return -1;
    }
    
    return 0;
}

bool device_parser_device_exists(const char *device_path) {
    if (!device_path) {
        return false;
    }
    
    struct stat st;
    return (stat(device_path, &st) == 0 && S_ISBLK(st.st_mode));
}

static int read_sysfs_file(const char *path, char *buf, size_t len) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    
    if (fgets(buf, len, f) == NULL) {
        fclose(f);
        return -1;
    }
    
    // Remove trailing newline
    size_t n = strlen(buf);
    if (n > 0 && buf[n-1] == '\n') {
        buf[n-1] = '\0';
    }
    
    fclose(f);
    return 0;
}

int device_parser_read_transport(const char *device_path, struct device_info *info) {
    if (!device_path || !info) {
        return -1;
    }
    
    // First parse the device path to get controller/namespace
    if (device_parser_parse_path(device_path, info) != 0) {
        return -1;
    }
    
    // Get controller sysfs path
    char controller_path[256];
    if (device_parser_get_controller_path(device_path, controller_path, sizeof(controller_path)) != 0) {
        return -1;
    }
    
    // Read transport type
    char transport_path[512];
    snprintf(transport_path, sizeof(transport_path), "%s/transport", controller_path);
    if (read_sysfs_file(transport_path, info->transport_type, sizeof(info->transport_type)) == 0) {
        // Normalize transport type
        if (strcmp(info->transport_type, "tcp") == 0 || strcmp(info->transport_type, "rdma") == 0) {
            // For TCP/RDMA, read additional info
            char addr_path[512];
            snprintf(addr_path, sizeof(addr_path), "%s/address", controller_path);
            char address[512];
            if (read_sysfs_file(addr_path, address, sizeof(address)) == 0) {
                // Parse address: traddr=192.168.1.100,trsvcid=4420,subsysnqn=nqn.xxx
                char *p = address;
                while (*p) {
                    if (strncmp(p, "traddr=", 7) == 0) {
                        p += 7;
                        char *end = strchr(p, ',');
                        if (end) {
                            size_t len = end - p;
                            if (len < sizeof(info->traddr)) {
                                strncpy(info->traddr, p, len);
                                info->traddr[len] = '\0';
                            }
                            p = end + 1;
                        } else {
                            strncpy(info->traddr, p, sizeof(info->traddr) - 1);
                            p += strlen(p);
                        }
                    } else if (strncmp(p, "trsvcid=", 8) == 0) {
                        p += 8;
                        char *end = strchr(p, ',');
                        if (end) {
                            size_t len = end - p;
                            if (len < sizeof(info->trsvcid)) {
                                strncpy(info->trsvcid, p, len);
                                info->trsvcid[len] = '\0';
                            }
                            p = end + 1;
                        } else {
                            strncpy(info->trsvcid, p, sizeof(info->trsvcid) - 1);
                            p += strlen(p);
                        }
                    } else if (strncmp(p, "subsysnqn=", 10) == 0) {
                        p += 10;
                        char *end = strchr(p, ',');
                        if (end) {
                            size_t len = end - p;
                            if (len < sizeof(info->subnqn)) {
                                strncpy(info->subnqn, p, len);
                                info->subnqn[len] = '\0';
                            }
                            p = end + 1;
                        } else {
                            strncpy(info->subnqn, p, sizeof(info->subnqn) - 1);
                            p += strlen(p);
                        }
                    } else {
                        // Skip to next comma or end
                        char *end = strchr(p, ',');
                        if (end) {
                            p = end + 1;
                        } else {
                            break;
                        }
                    }
                }
            }
        } else if (strcmp(info->transport_type, "pcie") == 0 || strcmp(info->transport_type, "PCIe") == 0) {
            // Normalize to lowercase
            strcpy(info->transport_type, "pcie");
            // For PCIe, read PCI address
            char addr_path[512];
            snprintf(addr_path, sizeof(addr_path), "%s/address", controller_path);
            char address[512];
            if (read_sysfs_file(addr_path, address, sizeof(address)) == 0) {
                // PCIe address format: 0000:01:00.0
                strncpy(info->traddr, address, sizeof(info->traddr) - 1);
                info->traddr[sizeof(info->traddr) - 1] = '\0';
            }
        }
    } else {
        // Default to PCIe if transport not found
        strcpy(info->transport_type, "pcie");
    }
    
    return 0;
}

