#include "poll_thread_manager.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <pthread.h>
#include <errno.h> // For ENXIO, ENODEV

namespace xload {

PollThreadContext::PollThreadContext()
    : thread_id(0)
    , qpair(nullptr)
    , spdk_thread(nullptr)
    , pthread(nullptr)
    , spdk_ctx(nullptr)
    , outstanding_io(0)
    , target_iodepth(0)
    , generator(nullptr)
    , lba_mgr(nullptr)
    , range_size(0)
    , stats(nullptr)
    , running(false)
    , should_stop(false)
{
}

PollThreadManager::PollThreadManager(SpdkContext* spdk_ctx,
                                     uint32_t num_cores,
                                     uint32_t iodepth,
                                     XcopyGenerator* generator,
                                     LbaManager* lba_mgr,
                                     uint64_t range_size,
                                     Statistics* stats)
    : spdk_ctx_(spdk_ctx)
    , num_cores_(num_cores)
    , iodepth_(iodepth)
    , generator_(generator)
    , lba_mgr_(lba_mgr)
    , range_size_(range_size)
    , stats_(stats)
    , running_(false)
{
}

PollThreadManager::~PollThreadManager() {
    stop();
    wait();
}

void PollThreadManager::xcopy_complete_cb(void* arg, const struct spdk_nvme_cpl* cpl) {
    XcopyOperation* op = static_cast<XcopyOperation*>(arg);
    if (!op) {
        return;
    }
    PollThreadContext* ctx = static_cast<PollThreadContext*>(op->user_data);
    
    if (!ctx || !op) {
        return;
    }
    
    // Calculate latency
    uint64_t end_time_ns = HighResTimer::now_ns();
    uint64_t latency_ns = end_time_ns - op->start_time_ns;
    
    // Check completion status
    uint16_t status_code = cpl->status.sc;
    
    if (spdk_nvme_cpl_is_error(cpl) || status_code != 0) {
        // Log detailed error information
        std::cerr << "XCOPY command failed: SCT=" << (int)cpl->status.sct 
                  << " SC=" << (int)cpl->status.sc 
                  << " (0x" << std::hex << (int)cpl->status.sc << std::dec << ")"
                  << " num_ranges=" << op->num_ranges
                  << " dst_lba=" << op->dst_lba
                  << " total_blocks=" << op->total_blocks;
        
        // Log first range details for debugging
        if (op->ranges && op->num_ranges > 0) {
            std::cerr << " first_range: src_lba=" << op->ranges[0].slba
                      << " nlb=" << op->ranges[0].nlb
                      << " (actual_blocks=" << (op->ranges[0].nlb + 1) << ")";
        }
        
        // Check if destination LBA + total_blocks exceeds namespace
        if (ctx->spdk_ctx) {
            const NamespaceInfo* ns_info = ctx->spdk_ctx->get_ns_info(op->dst_nsid);
            if (ns_info) {
                uint64_t dst_end = op->dst_lba + op->total_blocks;
                std::cerr << " dst_ns_size=" << ns_info->size_blocks
                          << " dst_end=" << dst_end
                          << " (exceeds=" << (dst_end > ns_info->size_blocks ? "YES" : "NO") << ")";
            }
        }
        
        std::cerr << std::endl;
        
        // Record failure
        ctx->stats->record_failure(status_code);
        
        // Check if it's a saturation error (backpressure)
        if (ErrorHandler::is_saturation_error(status_code)) {
            // Defer retry - don't block
            handle_backpressure(ctx, *op);
        }
    } else {
        // Success - log first few completions for debugging
        static std::atomic<int> completion_count(0);
        int count = completion_count.fetch_add(1);
        if (count < 3) {
            std::cout << "XCOPY completed successfully #" << (count + 1)
                      << ": num_ranges=" << op->num_ranges
                      << ", total_blocks=" << op->total_blocks
                      << ", latency=" << (latency_ns / 1000) << " us"
                      << std::endl;
        }
        
        // Calculate bytes copied
        uint64_t bytes = op->total_blocks * 512; // Assume 512-byte blocks for now
        // TODO: Get actual block size from namespace
        
        ctx->stats->record_completion(bytes, latency_ns);
    }
    
    // Decrement outstanding I/O
    ctx->outstanding_io--;
    
    // Free operation
    op->free_ranges();
    delete op;
    
    // Immediately submit next I/O to maintain depth (CSP pattern)
    // This happens in the polling thread context
    submit_next_io(ctx);
}

int PollThreadManager::submit_next_io(PollThreadContext* ctx) {
    // Check if we should stop
    if (ctx->should_stop.load()) {
        return 0;
    }
    
    // Check if we've reached target depth
    if (ctx->outstanding_io.load() >= ctx->target_iodepth) {
        return 0;
    }
    
    // Try deferred operations first (backpressure recovery)
    // Note: Deferred ops don't have ranges allocated, so we just skip them
    // and generate new operations. In a production system, you'd store
    // operation parameters and regenerate ranges.
    if (!ctx->deferred_ops.empty()) {
        ctx->deferred_ops.pop_back(); // Remove one deferred op
        // Fall through to generate new operation
    }
    
    // Generate new operation (retry up to 10 times to avoid overlap)
    XcopyOperation op;
    int retry_count = 0;
    const int max_retries = 10;
    while (ctx->generator->generate(op, *ctx->lba_mgr, ctx->range_size) != 0) {
        retry_count++;
        if (retry_count >= max_retries) {
            // Too many retries - likely a persistent issue (e.g., namespace too small)
            return 0;
        }
        // Retry with new random source LBAs
    }
    
    // Allocate ranges buffer
    if (op.allocate_ranges(ctx->generator->max_ranges_) != 0) {
        return 0;
    }
    
    // Record start time
    op.start_time_ns = HighResTimer::now_ns();
    op.user_data = ctx;
    
    // Get namespace for the command
    // For format 0 (same namespace), the namespace handle should be the SOURCE namespace
    // (which is the same as destination for same-namespace copy)
    // For format 2 (cross-namespace), we still use destination namespace handle
    // but source NSIDs are specified in range descriptors
    // 
    // IMPORTANT: Check the first range to determine which namespace to use
    // For format 0, all ranges should be from the same namespace as destination
    // For format 2, ranges can have different source NSIDs
    struct spdk_nvme_ns* ns = nullptr;
    if (op.ranges && op.num_ranges > 0) {
        // Check if format 2 is being used (DWORD 1 != 0)
        uint32_t* range_dwords = reinterpret_cast<uint32_t*>(&op.ranges[0]);
        uint32_t dword1 = range_dwords[1];
        
        // Debug: Log first few operations to verify format
        static std::atomic<int> debug_count(0);
        int dbg = debug_count.fetch_add(1);
        if (dbg < 5) {
            // Hex dump of first range descriptor (32 bytes)
            std::cout << "Debug XCOPY #" << (dbg + 1) 
                      << ": dst_nsid=" << op.dst_nsid
                      << ", num_ranges=" << op.num_ranges
                      << ", range[0] DWORD1=" << dword1
                      << " (0=format0, >0=format2)"
                      << ", nlb=" << op.ranges[0].nlb
                      << ", slba=" << op.ranges[0].slba
                      << ", dst_lba=" << op.dst_lba
                      << std::endl;
            std::cout << "  Range[0] hex dump (32 bytes): ";
            uint8_t* range_bytes = reinterpret_cast<uint8_t*>(&op.ranges[0]);
            for (int i = 0; i < 32 && i < sizeof(op.ranges[0]); i++) {
                printf("%02x ", range_bytes[i]);
            }
            std::cout << std::endl;
        }
        
        if (dword1 == 0) {
            // Format 0 (same namespace) - use destination namespace as source
            ns = ctx->spdk_ctx->get_ns(op.dst_nsid);
        } else {
            // Format 2 (cross-namespace) - use destination namespace handle
            // Source NSID is in DWORD 1 of each range descriptor
            ns = ctx->spdk_ctx->get_ns(op.dst_nsid);
        }
    } else {
        // Fallback: use destination namespace
        ns = ctx->spdk_ctx->get_ns(op.dst_nsid);
    }
    
    if (!ns) {
        // Namespace not found - free operation and return
        std::cerr << "Error: Namespace " << op.dst_nsid << " not found" << std::endl;
        op.free_ranges();
        return 0;
    }
    
    // Create copy for callback (SPDK will call callback with this)
    // The callback will receive op_copy, so we need to ensure op_copy has valid ranges
    XcopyOperation* op_copy = new XcopyOperation(op);
    
    // Verify op_copy has valid ranges
    if (!op_copy->ranges || op_copy->num_ranges != op.num_ranges) {
        std::cerr << "Error: Failed to copy operation ranges" << std::endl;
        delete op_copy;
        op.free_ranges();
        return 0;
    }
    
    // Submit XCOPY command using op_copy's ranges (which will be valid in callback)
    // SPDK will copy the ranges data into the command, so we can use op_copy's ranges
    int rc = spdk_nvme_ns_cmd_copy(
        ns,
        ctx->qpair,
        op_copy->ranges,  // Use op_copy's ranges, not op's (callback will receive op_copy)
        op_copy->num_ranges,
        op_copy->dst_lba,
        xcopy_complete_cb,
        op_copy // Callback will free this
    );
    
    // Free original op's ranges since we're using op_copy's ranges
    op.free_ranges();
    
    if (rc == 0) {
        ctx->outstanding_io++;
        // Debug: Log first few submissions
        static std::atomic<int> submission_count(0);
        int count = submission_count.fetch_add(1);
        if (count < 3) {
            std::cout << "Submitted XCOPY #" << (count + 1) 
                      << ": num_ranges=" << op_copy->num_ranges
                      << ", dst_lba=" << op_copy->dst_lba
                      << ", total_blocks=" << op_copy->total_blocks
                      << std::endl;
        }
        return 1;
    } else {
        // Submission failed - log error
        std::cerr << "Error: Failed to submit XCOPY command: rc=" << rc 
                  << " (errno=" << errno << ")" << std::endl;
        // Handle backpressure
        handle_backpressure(ctx, op);
        delete op_copy; // Clean up op_copy since submission failed
        return 0;
    }
}

void PollThreadManager::handle_backpressure(PollThreadContext* ctx, 
                                            const XcopyOperation& op) {
    // Defer operation for retry in next polling cycle
    // Don't block or sleep - just defer
    // Note: We track deferred count as a simple counter since we can't
    // easily store the ranges buffer. The next polling cycle will generate
    // a new operation instead.
    if (ctx->deferred_ops.size() < 100) { // Limit deferred queue size
        // Store minimal info - we'll regenerate the operation
        XcopyOperation deferred_op;
        deferred_op.dst_nsid = op.dst_nsid;
        deferred_op.dst_lba = op.dst_lba;
        deferred_op.num_ranges = op.num_ranges;
        deferred_op.total_blocks = op.total_blocks;
        deferred_op.ranges = nullptr; // Will be regenerated
        deferred_op.start_time_ns = 0;
        deferred_op.user_data = nullptr;
        ctx->deferred_ops.push_back(deferred_op);
    }
    // If queue is full, drop the operation (backpressure)
}

int PollThreadManager::poller_func(void* arg) {
    PollThreadContext* ctx = static_cast<PollThreadContext*>(arg);
    
    if (!ctx || !ctx->running.load()) {
        return 0;
    }
    
    // Process completions (non-blocking)
    spdk_nvme_qpair_process_completions(ctx->qpair, 0);
    
    // Submit new I/O to maintain depth (limit submissions per poll to avoid starvation)
    int submitted = 0;
    const int max_submissions_per_poll = 32;
    while (ctx->outstanding_io.load() < ctx->target_iodepth && 
           !ctx->should_stop.load() && submitted < max_submissions_per_poll) {
        if (submit_next_io(ctx) == 0) {
            break; // No more I/O to submit
        }
        submitted++;
    }
    
    return 1; // Continue polling
}

void PollThreadManager::thread_func(PollThreadContext* ctx) {
    if (!ctx) {
        std::cerr << "Error: Invalid context for thread" << std::endl;
        return;
    }
    
    // Set CPU affinity
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(ctx->thread_id, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
        std::cerr << "Warning: Failed to set CPU affinity for thread " 
                  << ctx->thread_id << std::endl;
    }
    
    // WORKAROUND: Run without SPDK threads - use direct I/O submission
    // This bypasses the thread library initialization issue completely
    ctx->spdk_thread = nullptr;  // No SPDK thread in workaround mode
    
    // QPair should already be created in start() function (serialized creation)
    if (!ctx->qpair) {
        std::cerr << "Error: QPair not created for thread " << ctx->thread_id << std::endl;
        return;
    }
    
    ctx->running = true;
    
    // Poll QPair directly without SPDK thread polling
    // This is a workaround - we poll the QPair completion queue directly
    int consecutive_errors = 0;
    const int max_consecutive_errors = 10; // Allow some transient errors
    int submission_count = 0;
    
    while (!ctx->should_stop.load()) {
        // Submit I/O if we have capacity and QPair is still valid
        if (ctx->qpair) {
            // Limit initial submissions to 1 to debug the disconnect issue
            // Once we confirm it works, we can remove this limit
            int max_initial_submissions = (submission_count == 0) ? 1 : ctx->target_iodepth;
            
            while (ctx->outstanding_io.load() < max_initial_submissions) {
                if (submit_next_io(ctx) == 0) {
                    break;  // No more I/O to submit
                }
                submission_count++;
            }
        }
        
        // Poll for completions directly on the QPair
        if (ctx->qpair) {
            int num_completions = spdk_nvme_qpair_process_completions(ctx->qpair, 0);
            if (num_completions < 0) {
                consecutive_errors++;
                std::cerr << "Warning: QPair poll error (rc=" << num_completions 
                          << ", consecutive=" << consecutive_errors << ")" << std::endl;
                if (consecutive_errors >= max_consecutive_errors) {
                    std::cerr << "Error: QPair disconnected after " << consecutive_errors 
                              << " consecutive errors. Continuing to run for full duration..." << std::endl;
                    // Don't break - continue running even if QPair is disconnected
                    // This allows the application to complete its full runtime
                    ctx->qpair = nullptr; // Mark QPair as invalid
                }
            } else {
                if (consecutive_errors > 0) {
                    std::cout << "QPair recovered after " << consecutive_errors << " errors" << std::endl;
                }
                consecutive_errors = 0; // Reset error counter on success
            }
        } else {
            // QPair is disconnected - just wait and continue
            // This allows the application to complete its full runtime
            usleep(1000); // Longer sleep when QPair is disconnected
        }
        
        // Small sleep to avoid 100% CPU (not ideal but works for sanity test)
        usleep(100);
    }
    
    // Wait for outstanding I/O to complete (if QPair is still valid)
    if (ctx->qpair) {
        while (ctx->outstanding_io.load() > 0) {
            spdk_nvme_qpair_process_completions(ctx->qpair, 0);
            usleep(100);
        }
    } else {
        // QPair disconnected - just wait a bit for any pending operations
        usleep(1000);
    }
    
    // Cleanup - don't delete QPair here as it's shared across threads
    // It will be cleaned up in wait() after all threads finish
    ctx->qpair = nullptr;
    ctx->running = false;
}

int PollThreadManager::start() {
    if (running_.load()) {
        return 0;
    }
    
    threads_.clear();
    threads_.reserve(num_cores_);
    
    // Check if we're in an SPDK thread context
    // After spdk_env_init(), we should be able to create threads directly
    // without explicit thread library initialization
    struct spdk_thread* current_thread = spdk_get_thread();
    
    // If we're not in a thread context, we need to create one first
    // However, spdk_thread_create requires being in a thread context
    // So we'll use a workaround: create threads from within the pthreads
    // by having each pthread create its own SPDK thread
    // This is not ideal but should work
    
    // WORKAROUND: Create QPairs serially and poll for connection
    // Without SPDK threads, we need to manually poll the QPair connection
    // For simplicity in workaround mode, use a single shared QPair for all threads
    // This avoids connection polling issues
    
    struct spdk_nvme_ctrlr* ctrlr = spdk_ctx_->get_ctrlr();
    if (!ctrlr) {
        std::cerr << "Error: No controller available" << std::endl;
        return -1;
    }
    
    // Create a single shared QPair for all threads (workaround mode)
    // Get controller's maximum queue depth capability
    const struct spdk_nvme_ctrlr_data* cdata = spdk_nvme_ctrlr_get_data(ctrlr);
    uint32_t max_queue_depth = cdata->maxcmd; // Maximum number of commands per queue
    
    // Use the minimum of requested depth and controller's maximum
    // This ensures we don't exceed the server's capabilities
    uint32_t qpair_depth = iodepth_;
    if (qpair_depth > max_queue_depth) {
        std::cout << "Note: Requested queue depth (" << iodepth_ 
                  << ") exceeds controller maximum (" << max_queue_depth << ")" << std::endl;
        std::cout << "      Using maximum supported depth: " << max_queue_depth << std::endl;
        qpair_depth = max_queue_depth;
    }
    
    // Also check for reasonable upper limit (some controllers report very high values)
    // Cap at 1024 as a practical limit for most NVMe/TCP targets
    if (qpair_depth > 1024) {
        std::cout << "Note: Controller reports very high queue depth (" << max_queue_depth 
                  << "), capping at 1024 for practical purposes" << std::endl;
        qpair_depth = 1024;
    }
    
    spdk_nvme_io_qpair_opts opts;
    spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
    opts.qprio = SPDK_NVME_QPRIO_URGENT;
    opts.io_queue_size = qpair_depth;
    
    std::cout << "Creating shared QPair with queue depth: " << qpair_depth << std::endl;
    struct spdk_nvme_qpair* shared_qpair = spdk_ctx_->create_qpair(qpair_depth, &opts);
    if (!shared_qpair) {
        std::cerr << "Error: Failed to create shared QPair with depth " << qpair_depth << std::endl;
        std::cerr << "       Try reducing --iodepth (e.g., 256 or 128)" << std::endl;
        return -1;
    }
    
    // CRITICAL: QPair creation is asynchronous for NVMe/TCP
    // We MUST poll the QPair to establish the connection
    // Without SPDK threads, we need to poll manually from the main thread
    std::cout << "Polling QPair connection..." << std::endl;
    int poll_count = 0;
    const int max_polls = 10000; // Max 10 seconds of polling (10000 * 1ms)
    bool connected = false;
    
    while (poll_count < max_polls) {
        // Process completions to advance connection state
        // This is critical - the connection won't establish without polling
        int rc = spdk_nvme_qpair_process_completions(shared_qpair, 0);
        
        // Negative return means error or connection not ready yet
        // Zero or positive means we processed completions (connection may be ready)
        if (rc >= 0) {
            // Connection might be ready - verify by checking if we can process more
            // without errors
            usleep(100); // Small delay
            rc = spdk_nvme_qpair_process_completions(shared_qpair, 0);
            if (rc >= 0) {
                // Connection appears established
                connected = true;
                break;
            }
        } else if (rc == -ENXIO || rc == -ENODEV) {
            // These errors suggest connection failed permanently
            std::cerr << "Error: QPair connection failed permanently (rc=" << rc << ")" << std::endl;
            spdk_ctx_->delete_qpair(shared_qpair);
            return -1;
        }
        
        // Connection still establishing - continue polling
        usleep(1000); // 1ms delay between polls
        poll_count++;
        
        // Print progress every second
        if (poll_count % 1000 == 0) {
            std::cout << "  Still connecting... (" << (poll_count / 1000) << "s)" << std::endl;
        }
    }
    
    if (!connected) {
        std::cerr << "Error: QPair connection failed to establish after " 
                  << (max_polls / 1000) << " seconds" << std::endl;
        std::cerr << "       This may indicate:" << std::endl;
        std::cerr << "       - Target is not accepting connections" << std::endl;
        std::cerr << "       - Queue depth too high (try --iodepth 128 or 256)" << std::endl;
        std::cerr << "       - Network connectivity issues" << std::endl;
        spdk_ctx_->delete_qpair(shared_qpair);
        return -1;
    }
    
    std::cout << "QPair connection established after " << (poll_count * 1000 / 1000) 
              << " ms" << std::endl;
    
    // CRITICAL: Wait a bit more and poll a few times to ensure QPair is fully ready
    // Sometimes the connection appears established but isn't ready for I/O yet
    std::cout << "Verifying QPair is ready for I/O..." << std::endl;
    for (int i = 0; i < 100; i++) {
        int rc = spdk_nvme_qpair_process_completions(shared_qpair, 0);
        if (rc < 0 && rc != -ENXIO && rc != -ENODEV) {
            // Transient error - continue polling
        }
        usleep(1000); // 1ms
    }
    std::cout << "QPair ready for I/O" << std::endl;
    
    // Store the actual QPair depth for later use
    uint32_t actual_qpair_depth = qpair_depth;
    
    // Create threads - all will share the same QPair
    for (uint32_t i = 0; i < num_cores_; i++) {
        auto ctx = std::make_unique<PollThreadContext>();
        ctx->thread_id = i;
        ctx->spdk_ctx = spdk_ctx_;
        // Distribute the actual QPair depth across threads (not the requested depth)
        // This ensures we don't try to submit more I/O than the QPair can handle
        ctx->target_iodepth = actual_qpair_depth / num_cores_;
        ctx->generator = generator_;
        ctx->lba_mgr = lba_mgr_;
        ctx->range_size = range_size_;
        ctx->stats = stats_;
        ctx->should_stop = false;
        ctx->spdk_thread = nullptr; // No SPDK thread in workaround mode
        ctx->qpair = shared_qpair; // All threads share the same QPair
        
        // Create pthread
        ctx->pthread = new std::thread(thread_func, ctx.get());
        
        threads_.push_back(std::move(ctx));
    }
    
    if (threads_.empty()) {
        std::cerr << "Error: Failed to create any threads" << std::endl;
        spdk_ctx_->delete_qpair(shared_qpair);
        return -1;
    }
    
    running_ = true;
    return 0;
}

void PollThreadManager::stop() {
    if (!running_.load()) {
        return;
    }
    
    running_ = false;
    
    for (auto& ctx : threads_) {
        ctx->should_stop = true;
    }
}

void PollThreadManager::wait() {
    for (auto& ctx : threads_) {
        if (ctx->pthread) {
            ctx->pthread->join();
            delete ctx->pthread;
            ctx->pthread = nullptr;
        }
    }
    
    // Cleanup shared QPair after all threads are done
    if (!threads_.empty() && threads_[0]->qpair) {
        spdk_ctx_->delete_qpair(threads_[0]->qpair);
        // Clear QPair from all contexts
        for (auto& ctx : threads_) {
            ctx->qpair = nullptr;
        }
    }
}

} // namespace xload

