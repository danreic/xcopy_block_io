#include "xcopy_generator.h"
#include <spdk/env.h>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <cstdlib>

namespace xload {

XcopyOperation::XcopyOperation()
    : dst_nsid(0)
    , dst_lba(0)
    , num_ranges(0)
    , ranges(nullptr)
    , total_blocks(0)
    , start_time_ns(0)
    , user_data(nullptr)
{
}

XcopyOperation::XcopyOperation(const XcopyOperation& other)
    : dst_nsid(other.dst_nsid)
    , dst_lba(other.dst_lba)
    , num_ranges(other.num_ranges)
    , ranges(nullptr)
    , total_blocks(other.total_blocks)
    , start_time_ns(other.start_time_ns)
    , user_data(other.user_data)
{
    // Deep copy ranges if they exist
    if (other.ranges && other.num_ranges > 0) {
        if (allocate_ranges(other.num_ranges) == 0 && ranges) {
            std::memcpy(ranges, other.ranges, 
                       other.num_ranges * sizeof(struct spdk_nvme_scc_source_range));
        }
    }
}

XcopyOperation& XcopyOperation::operator=(const XcopyOperation& other) {
    if (this == &other) {
        return *this;
    }
    
    free_ranges();
    
    dst_nsid = other.dst_nsid;
    dst_lba = other.dst_lba;
    num_ranges = other.num_ranges;
    total_blocks = other.total_blocks;
    start_time_ns = other.start_time_ns;
    user_data = other.user_data;
    
    if (other.ranges && other.num_ranges > 0) {
        if (allocate_ranges(other.num_ranges) == 0 && ranges) {
            std::memcpy(ranges, other.ranges,
                       other.num_ranges * sizeof(struct spdk_nvme_scc_source_range));
        }
    }
    
    return *this;
}

XcopyOperation::~XcopyOperation() {
    free_ranges();
}

int XcopyOperation::allocate_ranges(uint32_t max_ranges) {
    if (ranges != nullptr) {
        return 0; // Already allocated
    }
    
    size_t size = max_ranges * sizeof(struct spdk_nvme_scc_source_range);
    ranges = static_cast<struct spdk_nvme_scc_source_range*>(
        spdk_dma_malloc(size, 64, nullptr));
    
    if (!ranges) {
        return -1;
    }
    
    memset(ranges, 0, size);
    return 0;
}

void XcopyOperation::free_ranges() {
    if (ranges) {
        spdk_dma_free(ranges);
        ranges = nullptr;
    }
}

XcopyGenerator::XcopyGenerator(uint32_t max_ranges,
                               const std::vector<uint32_t>& src_nsids,
                               uint32_t dst_nsid,
                               const std::vector<const struct spdk_nvme_ns*>& namespaces,
                               bool enable_cross_namespace,
                               bool target_supports_cross_namespace)
    : max_ranges_(max_ranges)
    , src_nsids_(src_nsids)
    , dst_nsid_(dst_nsid)
    , namespaces_(namespaces)
    , enable_cross_namespace_(enable_cross_namespace)
    , target_supports_cross_namespace_(target_supports_cross_namespace)
    , rng_(std::chrono::steady_clock::now().time_since_epoch().count())
    , range_dist_(1, max_ranges)
{
}

uint32_t XcopyGenerator::get_random_src_nsid() {
    if (src_nsids_.empty()) {
        return dst_nsid_;
    }
    
    std::uniform_int_distribution<size_t> dist(0, src_nsids_.size() - 1);
    return src_nsids_[dist(rng_)];
}

uint64_t XcopyGenerator::get_ns_size(uint32_t nsid) {
    for (const auto* ns : namespaces_) {
        if (!ns) {
            continue; // Skip null namespace pointers
        }
        // Remove const for SPDK API calls
        struct spdk_nvme_ns* non_const_ns = const_cast<struct spdk_nvme_ns*>(ns);
        if (spdk_nvme_ns_get_id(non_const_ns) == nsid) {
            return spdk_nvme_ns_get_num_sectors(non_const_ns);
        }
    }
    return 0;
}

int XcopyGenerator::generate(XcopyOperation& op, LbaManager& lba_mgr, uint64_t range_size) {
    // Allocate ranges buffer if needed
    if (op.ranges == nullptr) {
        if (op.allocate_ranges(max_ranges_) != 0) {
            return -1;
        }
    }
    
    // Get random number of ranges
    op.num_ranges = get_random_num_ranges();
    op.dst_nsid = dst_nsid_;
    op.total_blocks = 0;
    op.start_time_ns = 0;
    op.user_data = nullptr;
    
    // We'll get the destination LBA after we calculate the actual total_blocks
    
    // For format 0 (same namespace), all ranges must use the destination namespace
    // For format 2 (cross-namespace), we can use different source namespaces
    // Determine which namespace to use for source ranges
    uint32_t effective_src_nsid = op.dst_nsid; // Default: same as destination (format 0)
    if (enable_cross_namespace_ && target_supports_cross_namespace_) {
        // Can use different source namespaces (format 2)
        effective_src_nsid = get_random_src_nsid();
    }
    
    // Get namespace size for validation
    uint64_t ns_size = get_ns_size(effective_src_nsid);
    if (ns_size == 0 || range_size >= ns_size) {
        return -1; // Invalid namespace or range too large
    }
    
    // Validate range_size is reasonable (max 0xFFFF blocks per range)
    if (range_size == 0 || range_size > 0x10000) {
        return -1; // Invalid range size
    }
    
    // Generate source ranges
    for (uint32_t i = 0; i < op.num_ranges; i++) {
        // For format 2, each range can have a different source NSID
        uint32_t src_nsid = effective_src_nsid;
        if (enable_cross_namespace_ && target_supports_cross_namespace_) {
            src_nsid = get_random_src_nsid();
            ns_size = get_ns_size(src_nsid);
            if (ns_size == 0 || range_size >= ns_size) {
                return -1; // Invalid namespace or range too large
            }
        }
        
        // Get random source LBA within namespace bounds
        uint64_t src_lba = lba_mgr.get_random_src_lba(range_size);
        
        // Validate source LBA + range_size doesn't exceed namespace
        if (src_lba + range_size > ns_size) {
            // Adjust src_lba to fit within namespace
            if (ns_size < range_size) {
                return -1; // Namespace too small
            }
            src_lba = ns_size - range_size;
        }
        
        // Fill in range descriptor
        // According to NVMe spec, range descriptor is 32 bytes (8 DWORDs):
        // - Format 0 (same namespace): 
        //   DWORD 0: Reserved (0)
        //   DWORD 1: Reserved (0)
        //   DWORD 2-3: Source LBA (64-bit, little-endian)
        //   DWORD 4: Reserved (0)
        //   DWORD 5: Number of Blocks (16-bit, 0-based) + Reserved (16-bit)
        //   DWORD 6-7: Destination LBA (64-bit, little-endian) - NOT in range descriptor!
        //
        // - Format 2 (cross-namespace):
        //   DWORD 0: Reserved (0)
        //   DWORD 1: Source NSID
        //   DWORD 2-3: Source LBA (64-bit, little-endian)
        //   DWORD 4: Reserved (0)
        //   DWORD 5: Number of Blocks (16-bit, 0-based) + Reserved (16-bit)
        //   DWORD 6-7: Destination LBA (64-bit, little-endian) - NOT in range descriptor!
        //
        // Note: Destination LBA is passed separately to spdk_nvme_ns_cmd_copy(), not in range descriptor
        // The range descriptor only contains source information
        
        // Clear the entire range descriptor
        memset(&op.ranges[i], 0, sizeof(op.ranges[i]));
        
        // Access as DWORDs to ensure correct layout
        uint32_t* dwords = reinterpret_cast<uint32_t*>(&op.ranges[i]);
        uint16_t* words = reinterpret_cast<uint16_t*>(&op.ranges[i]);
        
        // DWORD 0: Reserved (already 0 from memset)
        // DWORD 1: Source NSID (for format 2) or Reserved (0 for format 0)
        bool use_format2 = false;
        if (enable_cross_namespace_ && target_supports_cross_namespace_ && 
            src_nsid != op.dst_nsid) {
            // Format 2: Set source NSID in DWORD 1
            use_format2 = true;
            dwords[1] = src_nsid;
        }
        // Format 0: DWORD 1 remains 0 (already set by memset)
        
        // DWORD 2-3: Source LBA (64-bit, little-endian)
        // Store as two 32-bit values (little-endian)
        dwords[2] = src_lba & 0xFFFFFFFF;
        dwords[3] = (src_lba >> 32) & 0xFFFFFFFF;
        
        // DWORD 4: Reserved (already 0 from memset)
        
        // DWORD 5: Number of Blocks (16-bit, 0-based) in lower 16 bits, upper 16 bits reserved
        uint16_t nlb_value = (range_size > 0x10000) ? 0xFFFF : (range_size - 1);
        // DWORD 5 is at offset 20 bytes = 5 * 4 bytes
        // In little-endian: lower 16 bits first, then upper 16 bits
        words[10] = nlb_value; // Lower 16 bits of DWORD 5 (little-endian)
        // words[11] is upper 16 bits of DWORD 5 (Reserved), already 0 from memset
        
        // Also set using structure fields if they exist (for compatibility)
        op.ranges[i].slba = src_lba;
        op.ranges[i].nlb = nlb_value;
        op.ranges[i].eilbrt = 0;
        op.ranges[i].elbatm = 0;
        op.ranges[i].elbat = 0;
        
        // Update total_blocks with actual blocks (nlb + 1)
        op.total_blocks += (nlb_value + 1);
    }
    
    // CRITICAL: Get destination LBA and atomically advance by total_blocks
    // This ensures the next XCOPY command doesn't overlap with this one's destination
    // In XCOPY, all ranges are copied to consecutive destination LBAs starting from dst_lba
    // This must be atomic to prevent race conditions when multiple threads generate operations
    op.dst_lba = lba_mgr.get_and_advance_dst_lba(op.total_blocks);
    
    return 0;
}

} // namespace xload

