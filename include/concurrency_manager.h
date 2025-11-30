#ifndef CONCURRENCY_MANAGER_H
#define CONCURRENCY_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <spdk/nvme.h>
#include "xcopy_cmd.h"
#include "xcopy_tool.h"

// Thread context for worker threads
struct worker_thread {
    pthread_t thread;
    uint32_t thread_id;
    struct spdk_nvme_qpair *qpair;
    struct spdk_nvme_ctrlr *ctrlr;
    
    // Operation queue
    struct xcopy_operation *operations;
    uint32_t queue_depth;
    uint32_t queue_head;
    uint32_t queue_tail;
    uint32_t in_flight;
    
    // Synchronization
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
    
    // Statistics
    uint64_t ops_completed;
    uint64_t ops_failed;
    uint64_t bytes_copied;
    uint64_t total_latency_us;
    uint64_t min_latency_us;
    uint64_t max_latency_us;
    
    // Control
    bool running;
    bool should_stop;
};

// Concurrency manager context
struct concurrency_manager {
    struct worker_thread *workers;
    uint32_t num_workers;
    uint32_t queue_depth;
    
    // Global statistics
    pthread_mutex_t stats_mutex;
    struct xcopy_stats global_stats;
};

// Initialize concurrency manager
int concurrency_manager_init(struct concurrency_manager *cm,
                            struct spdk_nvme_ctrlr *ctrlr,
                            uint32_t num_threads,
                            uint32_t queue_depth);

// Start worker threads
int concurrency_manager_start(struct concurrency_manager *cm);

// Submit an operation
int concurrency_manager_submit(struct concurrency_manager *cm,
                              struct xcopy_operation *op);

// Stop worker threads
void concurrency_manager_stop(struct concurrency_manager *cm);

// Wait for all operations to complete
void concurrency_manager_wait(struct concurrency_manager *cm);

// Get statistics
void concurrency_manager_get_stats(struct concurrency_manager *cm,
                                  struct xcopy_stats *stats);

// Cleanup concurrency manager
void concurrency_manager_cleanup(struct concurrency_manager *cm);

#endif // CONCURRENCY_MANAGER_H

