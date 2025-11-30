#ifndef DEVICE_PARSER_H
#define DEVICE_PARSER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Device path information
struct device_info {
    char device_path[256];      // Full device path (e.g., /dev/nvme1n1)
    uint32_t controller_id;     // Controller number (e.g., 1 from nvme1)
    uint32_t namespace_id;      // Namespace number (e.g., 1 from n1)
    char transport_type[16];    // Transport type (tcp, pcie, rdma)
    char traddr[256];           // Transport address
    char trsvcid[16];           // Transport service ID
    char subnqn[256];           // Subsystem NQN
    bool valid;                 // Whether device info is valid
};

// Parse device path (e.g., /dev/nvme1n1)
// Returns 0 on success, -1 on error
int device_parser_parse_path(const char *device_path, struct device_info *info);

// Read transport information from sysfs
// Returns 0 on success, -1 on error
int device_parser_read_transport(const char *device_path, struct device_info *info);

// Get controller device path from namespace device path
// e.g., /dev/nvme1n1 -> /sys/class/nvme/nvme1
int device_parser_get_controller_path(const char *device_path, char *controller_path, size_t len);

// Check if device exists
bool device_parser_device_exists(const char *device_path);

#endif // DEVICE_PARSER_H

