#include "poll_thread_manager.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <pthread.h>
#include <errno.h> // For ENXIO, ENODEV
#include <chrono>  // For keep-alive timing

// SPDK thread functions are already declared in spdk/thread.h (included via spdk_context.h)

namespace xload {

// Helper for verbose logging
#define VERBOSE_LOG(msg) if (SpdkContext::is_verbose()) { std::cout << msg << std::endl; }

// Static members for shared qpair management
struct spdk_nvme_qpair* PollThreadManager::shared_qpair_ = nullptr;
::std::mutex PollThreadManager::qpair_mutex_;
uint32_t PollThreadManager::actual_qpair_depth_ = 0;

PollThreadContext::PollThreadContext()
    : thread_id(0)
    , qpair(nullptr)
    , spdk_thread(nullptr)
    , pthread(nullptr)
    , spdk_ctx(nullptr)
    , ctrlr_index(0)
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
        // Log errors only in verbose mode (limit logging)
        static std::atomic<uint64_t> error_count(0);
        uint64_t count = error_count.fetch_add(1);
        
        if (SpdkContext::is_verbose()) {
            bool should_log = (count < 5) || (count % 1000 == 0);
            
            if (should_log) {
                std::cerr << "XCOPY error #" << count << ": SC=" << (int)cpl->status.sc 
                          << " dst_lba=" << op->dst_lba << std::endl;
            }
        }
        
        // Record failure
        ctx->stats->record_failure(status_code);
        
        // Handle SC=8 (LBA Out of Range) - skip this LBA range
        // Some targets have restrictions on which LBAs can be used for XCOPY
        if (status_code == 0x8) {
            // Reset destination LBA to start to avoid hitting more invalid ranges
            // This is a workaround for targets with LBA restrictions
            if (ctx->lba_mgr) {
                ctx->lba_mgr->reset_dst_lba();
            }
        }
        
        // Check if it's a saturation error (backpressure)
        if (ErrorHandler::is_saturation_error(status_code)) {
            // Defer retry - don't block
            handle_backpressure(ctx, *op);
        }
    } else {
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
    
    // Check if qpair is disconnected
    if (!ctx->qpair) {
        // QPair is disconnected - can't submit new operations
        // Return 0 to indicate no submission, but don't break the loop
        // The poller will continue and may recover
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
    // For format 0 (same namespace), use destination namespace as source
    // For format 2 (cross-namespace), source NSIDs are specified in range descriptors
    struct spdk_nvme_ns* ns = ctx->spdk_ctx->get_ns(op.dst_nsid);
    
    if (!ns) {
        op.free_ranges();
        return 0;
    }
    
    // Create copy for callback (SPDK will call callback with this)
    // The callback will receive op_copy, so we need to ensure op_copy has valid ranges
    XcopyOperation* op_copy = new XcopyOperation(op);
    
    // Verify op_copy has valid ranges
    if (!op_copy->ranges || op_copy->num_ranges != op.num_ranges) {
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
        return 1;
    } else {
        handle_backpressure(ctx, op);
        delete op_copy;
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
    
    // Check if qpair needs reconnection
    if (!ctx->qpair) {
        struct spdk_nvme_qpair* new_qpair = reconnect_qpair(ctx, actual_qpair_depth_, ctx->ctrlr_index);
        if (new_qpair) {
            ctx->qpair = new_qpair;
            VERBOSE_LOG("Thread " << ctx->thread_id << ": QPair reconnected");
        } else {
            usleep(100000);
            return 1;
        }
    }
    
    // Process completions (non-blocking) - only if qpair is valid
    if (ctx->qpair) {
        spdk_nvme_qpair_process_completions(ctx->qpair, 0);
    }
    
    // Submit new I/O to maintain depth (limit submissions per poll to avoid starvation)
    // Only try to submit if qpair is valid
    if (ctx->qpair) {
        int submitted = 0;
        const int max_submissions_per_poll = 32;
        while (ctx->outstanding_io.load() < ctx->target_iodepth && 
               !ctx->should_stop.load() && submitted < max_submissions_per_poll) {
            if (submit_next_io(ctx) == 0) {
                break; // No more I/O to submit (could be qpair disconnected, depth reached, or generation failed)
            }
            submitted++;
        }
    }
    
    return 1; // Continue polling (even if qpair is disconnected, keep the loop running)
}

void PollThreadManager::thread_func(PollThreadContext* ctx) {
    if (!ctx) {
        return;
    }
    
    // Set CPU affinity
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(ctx->thread_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    
    // Check if SPDK threads are enabled
    bool use_spdk_threads = SpdkContext::is_spdk_threads_enabled();
    
    if (use_spdk_threads) {
        // Create SPDK thread for this worker
        char thread_name[32];
        snprintf(thread_name, sizeof(thread_name), "worker_%u", ctx->thread_id);
        
        ctx->spdk_thread = spdk_thread_create(thread_name, nullptr);
        if (!ctx->spdk_thread) {
            use_spdk_threads = false;
        } else {
            // Set this thread as the current SPDK thread
            spdk_set_thread(ctx->spdk_thread);
        }
    }
    
    if (!use_spdk_threads) {
        // WORKAROUND: Run without SPDK threads - use direct I/O submission
        ctx->spdk_thread = nullptr;
    }
    
    // QPair should already be created in start() function
    if (!ctx->qpair) {
        return;
    }
    
    ctx->running = true;
    
    // Poll QPair directly without SPDK thread polling
    // This is a workaround - we poll the QPair completion queue directly
    int consecutive_errors = 0;
    const int max_consecutive_errors = 10; // Allow some transient errors
    int submission_count = 0;
    int reconnect_attempts = 0;  // Per-thread reconnection counter
    const int max_reconnect_attempts = 10;
    const int reconnect_delay_ms = 5000;  // 5 seconds between attempts
    
    // Keep-alive: periodically poll admin queue to handle keep-alive commands
    auto last_admin_poll = std::chrono::steady_clock::now();
    const auto admin_poll_interval = std::chrono::milliseconds(1000); // Poll admin every 1 second
    
    while (!ctx->should_stop.load()) {
        // Poll SPDK thread if using SPDK threads
        if (use_spdk_threads && ctx->spdk_thread) {
            spdk_thread_poll(ctx->spdk_thread, 0, 0);
        }
        
        // Poll admin queue for keep-alive (required by NVMe-oF)
        // Each thread polls the admin queue of its assigned controller
        auto now = std::chrono::steady_clock::now();
        if (now - last_admin_poll >= admin_poll_interval) {
            struct spdk_nvme_ctrlr* ctrlr = ctx->spdk_ctx->get_ctrlr(ctx->ctrlr_index);
            if (ctrlr) {
                spdk_nvme_ctrlr_process_admin_completions(ctrlr);
            }
            last_admin_poll = now;
        }
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
                if (consecutive_errors >= max_consecutive_errors) {
                    ctx->spdk_ctx->delete_qpair(ctx->qpair);
                    ctx->qpair = nullptr;
                    reconnect_attempts = 0;
                }
            } else {
                consecutive_errors = 0;
            }
        } else {
            // QPair is disconnected - attempt to reconnect
            if (reconnect_attempts < max_reconnect_attempts) {
                reconnect_attempts++;
                usleep(reconnect_delay_ms * 1000);
                
                struct spdk_nvme_qpair* new_qpair = reconnect_qpair(ctx, ctx->target_iodepth, ctx->ctrlr_index);
                if (new_qpair) {
                    ctx->qpair = new_qpair;
                    consecutive_errors = 0;
                    reconnect_attempts = 0;
                }
            } else {
                usleep(1000);
            }
        }
        
        // Small sleep to avoid 100% CPU (not ideal but works for sanity test)
        usleep(100);
    }
    
    // Wait for outstanding I/O to complete (if QPair is still valid)
    if (ctx->qpair) {
        while (ctx->outstanding_io.load() > 0) {
            // Poll SPDK thread if using SPDK threads
            if (use_spdk_threads && ctx->spdk_thread) {
                spdk_thread_poll(ctx->spdk_thread, 0, 0);
            }
            spdk_nvme_qpair_process_completions(ctx->qpair, 0);
            usleep(100);
        }
    } else {
        // QPair disconnected - just wait a bit for any pending operations
        usleep(1000);
    }
    
    // Cleanup SPDK thread if used
    if (use_spdk_threads && ctx->spdk_thread) {
        spdk_thread_exit(ctx->spdk_thread);
        // Poll until thread has exited
        while (!spdk_thread_is_exited(ctx->spdk_thread)) {
            spdk_thread_poll(ctx->spdk_thread, 0, 0);
            usleep(100);
        }
        spdk_thread_destroy(ctx->spdk_thread);
        ctx->spdk_thread = nullptr;
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
    
    // Check if SPDK threads are enabled
    // If not, we use pthread-based multi-threading where each thread gets its own QPair
    // This avoids race conditions since each thread has exclusive access to its QPair
    bool use_spdk_threads = SpdkContext::is_spdk_threads_enabled();
    uint32_t effective_cores = num_cores_;
    
    if (!use_spdk_threads && num_cores_ > 1 && SpdkContext::is_verbose()) {
        std::cout << "Using pthread-based multi-threading" << std::endl;
    }
    
    threads_.clear();
    threads_.reserve(effective_cores);
    
    // Check if we're in an SPDK thread context
    // After spdk_env_init(), we should be able to create threads directly
    // without explicit thread library initialization
    struct spdk_thread* current_thread = spdk_get_thread();
    (void)current_thread; // Unused for now
    
    // Multi-path support: Distribute threads across all available controllers
    size_t num_controllers = spdk_ctx_->get_ctrlr_count();
    if (num_controllers == 0) {
        std::cerr << "Error: No controllers available" << std::endl;
        return -1;
    }
    
    // Get controller's maximum queue depth capability (from first controller)
    struct spdk_nvme_ctrlr* ctrlr = spdk_ctx_->get_ctrlr(0);
    const struct spdk_nvme_ctrlr_data* cdata = spdk_nvme_ctrlr_get_data(ctrlr);
    uint32_t max_queue_depth = cdata->maxcmd;
    
    // Calculate per-thread queue depth
    uint32_t per_thread_depth = iodepth_ / effective_cores;
    if (per_thread_depth < 4) per_thread_depth = 4;
    
    if (per_thread_depth > max_queue_depth) {
        per_thread_depth = max_queue_depth;
    }
    
    if (per_thread_depth > 1024) {
        per_thread_depth = 1024;
    }
    
    actual_qpair_depth_ = per_thread_depth;
    
    if (SpdkContext::is_verbose()) {
        std::cout << "Creating " << effective_cores << " QPairs (depth " 
                  << per_thread_depth << ") across " << num_controllers << " controller(s)" << std::endl;
    }
    
    // Track how many QPairs we've created on each controller
    std::vector<uint32_t> qpairs_per_ctrlr(num_controllers, 0);
    
    // Create threads - distribute across controllers round-robin
    for (uint32_t i = 0; i < effective_cores; i++) {
        auto ctx = std::make_unique<PollThreadContext>();
        ctx->thread_id = i;
        ctx->spdk_ctx = spdk_ctx_;
        ctx->target_iodepth = per_thread_depth;
        ctx->generator = generator_;
        ctx->lba_mgr = lba_mgr_;
        ctx->range_size = range_size_;
        ctx->stats = stats_;
        ctx->should_stop = false;
        ctx->spdk_thread = nullptr;
        
        // Try to create QPair on each controller (round-robin with fallback)
        bool qpair_created = false;
        size_t attempts = 0;
        size_t ctrlr_idx = i % num_controllers;  // Start with round-robin
        
        while (!qpair_created && attempts < num_controllers) {
            struct spdk_nvme_ctrlr* target_ctrlr = spdk_ctx_->get_ctrlr(ctrlr_idx);
            spdk_nvme_io_qpair_opts opts;
            spdk_nvme_ctrlr_get_default_io_qpair_opts(target_ctrlr, &opts, sizeof(opts));
            opts.qprio = SPDK_NVME_QPRIO_URGENT;
            opts.io_queue_size = per_thread_depth;
            
            ctx->qpair = spdk_ctx_->create_qpair(per_thread_depth, &opts, ctrlr_idx);
            if (ctx->qpair) {
                ctx->ctrlr_index = ctrlr_idx;  // Track which controller this thread uses
                qpairs_per_ctrlr[ctrlr_idx]++;
                qpair_created = true;
            } else {
                // This controller is full, try next one
                ctrlr_idx = (ctrlr_idx + 1) % num_controllers;
                attempts++;
            }
        }
        
        if (!qpair_created) {
            if (threads_.empty()) {
                std::cerr << "Error: Failed to create any QPairs" << std::endl;
                return -1;
            }
            break;
        }
        
        // Poll QPair to establish connection
        int poll_count = 0;
        const int max_polls = 5000; // 5 seconds max per QPair
        bool connected = false;
        
        while (poll_count < max_polls) {
            int rc = spdk_nvme_qpair_process_completions(ctx->qpair, 0);
            if (rc >= 0) {
                usleep(100);
                rc = spdk_nvme_qpair_process_completions(ctx->qpair, 0);
                if (rc >= 0) {
                    connected = true;
                    break;
                }
            } else if (rc == -ENXIO || rc == -ENODEV) {
                spdk_ctx_->delete_qpair(ctx->qpair);
                for (auto& t : threads_) {
                    if (t->qpair) spdk_ctx_->delete_qpair(t->qpair);
                }
                std::cerr << "Error: QPair connection failed" << std::endl;
                return -1;
            }
            usleep(1000);
            poll_count++;
        }
        
        if (!connected) {
            spdk_ctx_->delete_qpair(ctx->qpair);
            for (auto& t : threads_) {
                if (t->qpair) spdk_ctx_->delete_qpair(t->qpair);
            }
            std::cerr << "Error: QPair connection timeout" << std::endl;
            return -1;
        }
        
        // Brief verification poll
        for (int j = 0; j < 10; j++) {
            spdk_nvme_qpair_process_completions(ctx->qpair, 0);
            usleep(1000);
        }
        
        // Create pthread for this context
        ctx->pthread = new std::thread(thread_func, ctx.get());
        threads_.push_back(std::move(ctx));
    }
    
    if (threads_.empty()) {
        std::cerr << "Error: Failed to create any threads" << std::endl;
        return -1;
    }
    
    // Show summary (always shown)
    std::cout << "Started " << threads_.size() << " worker(s)";
    if (num_controllers > 1) {
        std::cout << " across " << num_controllers << " controllers";
    }
    std::cout << std::endl;
    
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
    
    // Cleanup each thread's QPair
    {
        std::lock_guard<std::mutex> lock(qpair_mutex_);
        for (auto& ctx : threads_) {
            if (ctx->qpair) {
                spdk_ctx_->delete_qpair(ctx->qpair);
                ctx->qpair = nullptr;
            }
        }
        shared_qpair_ = nullptr; // Clear legacy shared reference
    }
}

struct spdk_nvme_qpair* PollThreadManager::reconnect_qpair(PollThreadContext* ctx, uint32_t qpair_depth, size_t ctrlr_index) {
    if (!ctx || !ctx->spdk_ctx) {
        return nullptr;
    }
    
    struct spdk_nvme_ctrlr* ctrlr = ctx->spdk_ctx->get_ctrlr(ctrlr_index);
    if (!ctrlr) {
        return nullptr;
    }
    
    spdk_nvme_io_qpair_opts opts;
    spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
    opts.qprio = SPDK_NVME_QPRIO_URGENT;
    opts.io_queue_size = qpair_depth;
    
    struct spdk_nvme_qpair* new_qpair = ctx->spdk_ctx->create_qpair(qpair_depth, &opts, ctrlr_index);
    if (!new_qpair) {
        return nullptr;
    }
    
    // Poll for connection (same logic as in start())
    int poll_count = 0;
    const int max_polls = 10000; // Max 10 seconds
    bool connected = false;
    
    while (poll_count < max_polls) {
        int rc = spdk_nvme_qpair_process_completions(new_qpair, 0);
        
        if (rc >= 0) {
            usleep(100);
            rc = spdk_nvme_qpair_process_completions(new_qpair, 0);
            if (rc >= 0) {
                connected = true;
                break;
            }
        } else if (rc == -ENXIO || rc == -ENODEV) {
            ctx->spdk_ctx->delete_qpair(new_qpair);
            return nullptr;
        }
        
        usleep(1000);
        poll_count++;
    }
    
    if (!connected) {
        ctx->spdk_ctx->delete_qpair(new_qpair);
        return nullptr;
    }
    
    // Verify qpair is ready
    for (int i = 0; i < 100; i++) {
        spdk_nvme_qpair_process_completions(new_qpair, 0);
        usleep(1000);
    }
    
    return new_qpair;
}

} // namespace xload

