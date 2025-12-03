#ifndef POLL_THREAD_MANAGER_H
#define POLL_THREAD_MANAGER_H

#include <spdk/nvme.h>
#include <spdk/thread.h>
#include <spdk/env.h>
#include <stdint.h>
#include <atomic>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include "spdk_context.h"
#include "xcopy_generator.h"
#include "lba_manager.h"
#include "statistics.h"
#include "error_handler.h"
#include "high_res_timer.h"

namespace xload {

struct PollThreadContext {
    uint32_t thread_id;
    struct spdk_nvme_qpair* qpair;
    struct spdk_thread* spdk_thread;
    std::thread* pthread;
    SpdkContext* spdk_ctx;
    
    // I/O state
    std::atomic<uint32_t> outstanding_io;
    uint32_t target_iodepth;
    
    // Workload generation
    XcopyGenerator* generator;
    LbaManager* lba_mgr;
    uint64_t range_size;
    
    // Statistics
    Statistics* stats;
    
    // Control
    std::atomic<bool> running;
    std::atomic<bool> should_stop;
    
    // Deferred submissions (for backpressure)
    std::vector<XcopyOperation> deferred_ops;
    
    PollThreadContext();
};

class PollThreadManager {
public:
    PollThreadManager(SpdkContext* spdk_ctx, 
                     uint32_t num_cores,
                     uint32_t iodepth,
                     XcopyGenerator* generator,
                     LbaManager* lba_mgr,
                     uint64_t range_size,
                     Statistics* stats);
    
    ~PollThreadManager();
    
    // Initialize and start threads
    int start();
    
    // Stop threads
    void stop();
    
    // Wait for completion
    void wait();
    
    // Check if running
    bool is_running() const { return running_; }
    
private:
    SpdkContext* spdk_ctx_;
    uint32_t num_cores_;
    uint32_t iodepth_;
    XcopyGenerator* generator_;
    LbaManager* lba_mgr_;
    uint64_t range_size_;
    Statistics* stats_;
    
    std::vector<std::unique_ptr<PollThreadContext>> threads_;
    std::atomic<bool> running_;
    
    // Thread function
    static void thread_func(PollThreadContext* ctx);
    
    // SPDK thread poller function
    static int poller_func(void* arg);
    
    // XCOPY completion callback
    static void xcopy_complete_cb(void* arg, const struct spdk_nvme_cpl* cpl);
    
    // Submit next I/O operation
    static int submit_next_io(PollThreadContext* ctx);
    
    // Handle backpressure
    static void handle_backpressure(PollThreadContext* ctx, const XcopyOperation& op);
    
    // Reconnect qpair (called when disconnection is detected)
    static struct spdk_nvme_qpair* reconnect_qpair(PollThreadContext* ctx, uint32_t qpair_depth);
    
    // Shared qpair for all threads (protected by mutex)
    static struct spdk_nvme_qpair* shared_qpair_;
    static std::mutex qpair_mutex_;
    static uint32_t actual_qpair_depth_;

} // namespace xload

#endif // POLL_THREAD_MANAGER_H

