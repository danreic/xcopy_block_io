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
    op.dst_lba = lba_mgr.get_next_dst_lba(range_size);
    op.total_blocks = 0;
    op.start_time_ns = 0;
    op.user_data = nullptr;
    
    // Generate source ranges
    for (uint32_t i = 0; i < op.num_ranges; i++) {
        uint32_t src_nsid = get_random_src_nsid();
        uint64_t ns_size = get_ns_size(src_nsid);
        
        if (ns_size == 0 || range_size >= ns_size) {
            return -1; // Invalid namespace or range too large
        }
        
        // Validate range_size is reasonable (max 0xFFFF blocks per range)
        if (range_size == 0 || range_size > 0x10000) {
            return -1; // Invalid range size
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
        // Note: SPDK's spdk_nvme_scc_source_range structure format
        // According to NVMe spec:
        // - Format 0 (same namespace): DWORD 0-1: Reserved (must be 0), DWORD 2-3: Source LBA,
        //                              DWORD 4: Reserved, DWORD 5: Number of Blocks (0-based)
        // - Format 2 (cross-namespace): DWORD 0: Reserved, DWORD 1: Source NSID,
        //                               DWORD 2-3: Source LBA, DWORD 4: Reserved,
        //                               DWORD 5: Number of Blocks (0-based)
        //
        // SPDK's spdk_nvme_ns_cmd_copy() expects format 0 (same namespace) by default.
        // The namespace handle passed to the function is the source namespace.
        // For format 2 (cross-namespace), we need to set the source NSID in DWORD 1.
        memset(&op.ranges[i], 0, sizeof(op.ranges[i]));
        op.ranges[i].slba = src_lba;
        op.ranges[i].nlb = range_size - 1; // 0-based (0 = 1 block, 2047 = 2048 blocks)
        op.ranges[i].eilbrt = 0; // Expected initial logical block reference tag
        op.ranges[i].elbatm = 0; // Expected LBA application tag mask
        op.ranges[i].elbat = 0;  // Expected LBA application tag
        
        // Determine if we should use format 2 (cross-namespace) or format 0 (same-namespace)
        bool use_format2 = false;
        if (enable_cross_namespace_ && target_supports_cross_namespace_ && 
            src_nsid != op.dst_nsid) {
            // Cross-namespace copy: use format 2, set source NSID in DWORD 1
            use_format2 = true;
            uint32_t* range_dwords = reinterpret_cast<uint32_t*>(&op.ranges[i]);
            range_dwords[1] = src_nsid; // DWORD 1 = Source NSID (format 2)
        }
        // If use_format2 is false, DWORD 1 remains 0 (format 0, same-namespace)
        
        op.total_blocks += range_size;
    }
    
    return 0;
}

} // namespace xload

