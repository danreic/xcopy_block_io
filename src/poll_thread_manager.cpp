#include "poll_thread_manager.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <chrono>
#include <algorithm>
#include <sched.h>

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
    , op_pool(nullptr)
    , deferred_count(0)
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

// Optimized completion callback using pooled operations
void PollThreadManager::xcopy_complete_cb_pooled(void* arg, const struct spdk_nvme_cpl* cpl) {
    PooledOperation* op = static_cast<PooledOperation*>(arg);
    if (!op) {
        return;
    }
    PollThreadContext* ctx = static_cast<PollThreadContext*>(op->user_data);
    
    if (!ctx) {
        return;
    }
    
    // Calculate latency
    uint64_t end_time_ns = HighResTimer::now_ns();
    uint64_t latency_ns = end_time_ns - op->start_time_ns;
    
    // Check completion status
    uint16_t status_code = cpl->status.sc;
    
    if (spdk_nvme_cpl_is_error(cpl) || status_code != 0) {
        // Record failure (lock-free)
        ctx->stats->record_failure(status_code);
        
        // Handle specific errors
        if (status_code == 0x8 && ctx->lba_mgr) {
            ctx->lba_mgr->reset_dst_lba();
        }
        
        if (ErrorHandler::is_saturation_error(status_code)) {
            ctx->deferred_count.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        // Record completion (lock-free)
        uint64_t bytes = op->total_blocks * 512;
        ctx->stats->record_completion(bytes, latency_ns);
    }
    
    // Decrement outstanding I/O
    ctx->outstanding_io.fetch_sub(1, std::memory_order_release);
    
    // Release operation back to pool (lock-free)
    if (ctx->op_pool) {
        ctx->op_pool->release(op);
    }
    
    // Submit next I/O immediately (CSP pattern)
    submit_next_io_pooled(ctx);
}

// Optimized submit using pooled operations - NO MALLOC on hot path
int PollThreadManager::submit_next_io_pooled(PollThreadContext* ctx) {
    // Fast path checks
    if (ctx->should_stop.load(std::memory_order_relaxed)) {
        return 0;
    }
    
    if (!ctx->qpair) {
        return 0;
    }
    
    if (ctx->outstanding_io.load(std::memory_order_relaxed) >= ctx->target_iodepth) {
        return 0;
    }
    
    // Handle deferred operations
    uint32_t deferred = ctx->deferred_count.load(std::memory_order_relaxed);
    if (deferred > 0) {
        ctx->deferred_count.fetch_sub(1, std::memory_order_relaxed);
    }
    
    // Acquire operation from pool (lock-free)
    PooledOperation* op = ctx->op_pool->acquire();
    if (!op) {
        return 0; // Pool exhausted
    }
    
    // Create temporary XcopyOperation for generate() compatibility
    XcopyOperation temp_op;
    temp_op.ranges = op->ranges;  // Use pooled DMA buffer
    temp_op.num_ranges = 0;
    
    // Generate operation (retry on overlap)
    int retry_count = 0;
    while (ctx->generator->generate(temp_op, *ctx->lba_mgr, ctx->range_size) != 0) {
        if (++retry_count >= 10) {
            ctx->op_pool->release(op);
            return 0;
        }
    }
    
    // Copy generated data to pooled operation
    op->dst_nsid = temp_op.dst_nsid;
    op->dst_lba = temp_op.dst_lba;
    op->num_ranges = temp_op.num_ranges;
    op->total_blocks = temp_op.total_blocks;
    op->start_time_ns = HighResTimer::now_ns();
    op->user_data = ctx;
    
    // Don't free temp_op.ranges - it points to pooled buffer
    temp_op.ranges = nullptr;
    
    // Get namespace
    struct spdk_nvme_ns* ns = ctx->spdk_ctx->get_ns(op->dst_nsid);
    if (!ns) {
        ctx->op_pool->release(op);
        return 0;
    }
    
    // Submit XCOPY command
    int rc = spdk_nvme_ns_cmd_copy(
        ns,
        ctx->qpair,
        op->ranges,
        op->num_ranges,
        op->dst_lba,
        xcopy_complete_cb_pooled,
        op
    );
    
    if (rc == 0) {
        ctx->outstanding_io.fetch_add(1, std::memory_order_release);
        return 1;
    } else {
        ctx->op_pool->release(op);
        handle_backpressure_pooled(ctx);
        return 0;
    }
}

void PollThreadManager::handle_backpressure_pooled(PollThreadContext* ctx) {
    // Just increment deferred counter - no allocation
    if (ctx->deferred_count.load(std::memory_order_relaxed) < 100) {
        ctx->deferred_count.fetch_add(1, std::memory_order_relaxed);
    }
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
    
    // Submit new I/O to maintain depth
    // Only try to submit if qpair is valid
    // Removed fixed limit - allow up to target_iodepth submissions per poll for faster queue filling
    if (ctx->qpair) {
        int submitted = 0;
        // Make limit proportional to iodepth to avoid starvation with high iodepth
        // Cap at reasonable maximum to prevent excessive CPU usage in error cases
        const int max_submissions_per_poll = std::min(static_cast<int>(ctx->target_iodepth), 256);
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
    CPU_SET(ctx->thread_id % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    
    // Check if SPDK threads are enabled
    bool use_spdk_threads = SpdkContext::is_spdk_threads_enabled();
    
    if (use_spdk_threads) {
        char thread_name[32];
        snprintf(thread_name, sizeof(thread_name), "worker_%u", ctx->thread_id);
        ctx->spdk_thread = spdk_thread_create(thread_name, nullptr);
        if (ctx->spdk_thread) {
            spdk_set_thread(ctx->spdk_thread);
        } else {
            use_spdk_threads = false;
        }
    }
    
    if (!ctx->qpair) {
        return;
    }
    
    ctx->running = true;
    
    int consecutive_errors = 0;
    const int max_consecutive_errors = 10;
    int reconnect_attempts = 0;
    const int max_reconnect_attempts = 10;
    
    // Admin poll timing - less frequent to reduce overhead
    uint64_t last_admin_poll_ns = HighResTimer::now_ns();
    const uint64_t admin_poll_interval_ns = 1000000000ULL; // 1 second
    
    // Batch counter for adaptive polling
    int idle_cycles = 0;
    
    while (!ctx->should_stop.load(std::memory_order_relaxed)) {
        // Poll SPDK thread if enabled
        if (use_spdk_threads && ctx->spdk_thread) {
            spdk_thread_poll(ctx->spdk_thread, 0, 0);
        }
        
        // Admin poll - only check time occasionally
        uint64_t now_ns = HighResTimer::now_ns();
        if (now_ns - last_admin_poll_ns >= admin_poll_interval_ns) {
            struct spdk_nvme_ctrlr* ctrlr = ctx->spdk_ctx->get_ctrlr(ctx->ctrlr_index);
            if (ctrlr) {
                spdk_nvme_ctrlr_process_admin_completions(ctrlr);
            }
            last_admin_poll_ns = now_ns;
        }
        
        if (ctx->qpair) {
            // OPTIMIZED: Batch submit multiple operations
            int submitted = 0;
            uint32_t outstanding = ctx->outstanding_io.load(std::memory_order_relaxed);
            while (outstanding < ctx->target_iodepth && submitted < 32) {
                if (submit_next_io_pooled(ctx) == 0) {
                    break;
                }
                submitted++;
                outstanding = ctx->outstanding_io.load(std::memory_order_relaxed);
            }
            
            // Process completions - process all available
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
                
                // Adaptive backoff - only sleep if truly idle
                if (num_completions == 0 && submitted == 0) {
                    idle_cycles++;
                    if (idle_cycles > 1000) {
                        // Only sleep after prolonged idleness
                        usleep(1);
                        idle_cycles = 0;
                    }
                } else {
                    idle_cycles = 0;
                }
            }
        } else {
            // QPair disconnected - attempt reconnect
            if (reconnect_attempts < max_reconnect_attempts) {
                reconnect_attempts++;
                usleep(5000000); // 5 seconds
                
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
    }
    
    // Drain outstanding I/O
    if (ctx->qpair) {
        int drain_cycles = 0;
        while (ctx->outstanding_io.load(std::memory_order_relaxed) > 0 && drain_cycles < 10000) {
            if (use_spdk_threads && ctx->spdk_thread) {
                spdk_thread_poll(ctx->spdk_thread, 0, 0);
            }
            spdk_nvme_qpair_process_completions(ctx->qpair, 0);
            usleep(100);
            drain_cycles++;
        }
    }
    
    // Cleanup SPDK thread
    if (use_spdk_threads && ctx->spdk_thread) {
        spdk_thread_exit(ctx->spdk_thread);
        while (!spdk_thread_is_exited(ctx->spdk_thread)) {
            spdk_thread_poll(ctx->spdk_thread, 0, 0);
            usleep(100);
        }
        spdk_thread_destroy(ctx->spdk_thread);
        ctx->spdk_thread = nullptr;
    }
    
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
        ctx->deferred_count = 0;
        
        // Initialize per-thread operation pool
        // Pool size = 2x target depth to handle burst + deferred ops
        ctx->op_pool = new OperationPool();
        if (ctx->op_pool->init(per_thread_depth * 2, generator_->max_ranges_) != 0) {
            std::cerr << "Error: Failed to initialize operation pool for thread " << i << std::endl;
            delete ctx->op_pool;
            ctx->op_pool = nullptr;
            return -1;
        }
        
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
    
    // Cleanup each thread's resources
    {
        std::lock_guard<std::mutex> lock(qpair_mutex_);
        for (auto& ctx : threads_) {
            if (ctx->qpair) {
                spdk_ctx_->delete_qpair(ctx->qpair);
                ctx->qpair = nullptr;
            }
            // Cleanup operation pool
            if (ctx->op_pool) {
                delete ctx->op_pool;
                ctx->op_pool = nullptr;
            }
        }
        shared_qpair_ = nullptr;
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

