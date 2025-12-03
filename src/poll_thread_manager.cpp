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
        // Record failure
        ctx->stats->record_failure(status_code);
        
        // Check if it's a saturation error (backpressure)
        if (ErrorHandler::is_saturation_error(status_code)) {
            // Defer retry - don't block
            handle_backpressure(ctx, *op);
        }
    } else {
        // Success - calculate bytes copied
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
    
    // Generate new operation
    XcopyOperation op;
    if (ctx->generator->generate(op, *ctx->lba_mgr, ctx->range_size) != 0) {
        return 0;
    }
    
    // Allocate ranges buffer
    if (op.allocate_ranges(ctx->generator->max_ranges_) != 0) {
        return 0;
    }
    
    // Record start time
    op.start_time_ns = HighResTimer::now_ns();
    op.user_data = ctx;
    
    // Get namespace - check for null
    struct spdk_nvme_ns* ns = ctx->spdk_ctx->get_ns(op.dst_nsid);
    if (!ns) {
        // Namespace not found - free operation and return
        op.free_ranges();
        return 0;
    }
    
    // Create copy for callback (SPDK will call callback with this)
    XcopyOperation* op_copy = new XcopyOperation(op);
    
    // Submit XCOPY command
    int rc = spdk_nvme_ns_cmd_copy(
        ns,
        ctx->qpair,
        op.ranges,
        op.num_ranges,
        op.dst_lba,
        xcopy_complete_cb,
        op_copy // Callback will free this
    );
    
    if (rc == 0) {
        ctx->outstanding_io++;
        return 1;
    } else {
        // Submission failed - handle backpressure
        handle_backpressure(ctx, op);
        op.free_ranges();
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
    while (!ctx->should_stop.load()) {
        // Submit I/O if we have capacity
        while (ctx->outstanding_io.load() < ctx->target_iodepth) {
            if (submit_next_io(ctx) == 0) {
                break;  // No more I/O to submit
            }
        }
        
        // Poll for completions directly on the QPair
        int num_completions = spdk_nvme_qpair_process_completions(ctx->qpair, 0);
        if (num_completions < 0) {
            break;  // Error
        }
        
        // Small sleep to avoid 100% CPU (not ideal but works for sanity test)
        usleep(100);
    }
    
    // Wait for outstanding I/O to complete
    while (ctx->outstanding_io.load() > 0) {
        spdk_nvme_qpair_process_completions(ctx->qpair, 0);
        usleep(100);
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

