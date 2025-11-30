#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "concurrency_manager.h"
#include "xcopy_cmd.h"
#include "statistics.h"
#include "nvme_wrapper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <stdint.h>
#include <limits.h>

// Completion callback for NVMe commands (called from io_uring wrapper)
static void xcopy_completion_cb(void *user_data, int result, uint32_t status) {
    struct xcopy_operation *op = (struct xcopy_operation *)user_data;
    struct worker_thread *worker = (struct worker_thread *)op->user_data;
    
    if (!op || !worker) {
        return;
    }
    
    // Calculate latency
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t end_time_us = (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    uint64_t latency_us = end_time_us - op->start_time_us;
    
    // Update statistics
    if (result != 0 || status != 0) {
        worker->ops_failed++;
        op->status = -1;
    } else {
        worker->ops_completed++;
        op->status = 0;
        
        // Calculate bytes copied (sum of all ranges)
        uint64_t bytes_copied = 0;
        for (uint32_t i = 0; i < op->num_ranges; i++) {
            uint64_t blocks = (uint64_t)op->ranges[i].num_blocks + 1;
            // Get block size from namespace (default to 512 if not available)
            uint32_t block_size = nvme_wrapper_get_block_size(worker->nvme_ctx, op->ranges[i].src_nsid);
            if (block_size == 0) {
                block_size = 512;  // Default
            }
            bytes_copied += blocks * block_size;
        }
        
        worker->bytes_copied += bytes_copied;
        worker->total_latency_us += latency_us;
        
        if (latency_us < worker->min_latency_us || worker->min_latency_us == 0) {
            worker->min_latency_us = latency_us;
        }
        
        if (latency_us > worker->max_latency_us) {
            worker->max_latency_us = latency_us;
        }
    }
    
    // Mark operation as completed
    op->completed = true;
    
    // Decrement in-flight count
    pthread_mutex_lock(&worker->queue_mutex);
    worker->in_flight--;
    pthread_cond_signal(&worker->queue_cond);
    pthread_mutex_unlock(&worker->queue_mutex);
}

// Worker thread function
static void *worker_thread_func(void *arg) {
    struct worker_thread *worker = (struct worker_thread *)arg;
    
    if (!worker) {
        return NULL;
    }
    
    worker->running = true;
    
    while (worker->running && !worker->should_stop) {
        // Process io_uring completions (non-blocking)
        io_uring_nvme_process_completions(&worker->io_uring_ctx, 32);
        
        // Small sleep to avoid busy-waiting
        usleep(10);
    }
    
    // Process any remaining completions
    while (worker->in_flight > 0) {
        io_uring_nvme_process_completions(&worker->io_uring_ctx, 32);
        usleep(100);
    }
    
    worker->running = false;
    return NULL;
}

int concurrency_manager_init(struct concurrency_manager *cm,
                            struct nvme_context *nvme_ctx,
                            uint32_t num_threads,
                            uint32_t queue_depth) {
    if (!cm || !nvme_ctx || num_threads == 0 || queue_depth == 0) {
        return -EINVAL;
    }
    
    memset(cm, 0, sizeof(*cm));
    
    cm->num_workers = num_threads;
    cm->queue_depth = queue_depth;
    
    // Allocate worker threads
    cm->workers = calloc(cm->num_workers, sizeof(struct worker_thread));
    if (!cm->workers) {
        return -ENOMEM;
    }
    
    // Initialize mutex for statistics
    if (pthread_mutex_init(&cm->stats_mutex, NULL) != 0) {
        free(cm->workers);
        return -1;
    }
    
    // Initialize each worker thread
    for (uint32_t i = 0; i < cm->num_workers; i++) {
        struct worker_thread *worker = &cm->workers[i];
        
        worker->thread_id = i;
        worker->nvme_ctx = nvme_ctx;
        
        // Initialize io_uring context
        int ret = io_uring_nvme_init(&worker->io_uring_ctx, queue_depth);
        if (ret != 0) {
            // Cleanup on failure
            for (uint32_t j = 0; j < i; j++) {
                io_uring_nvme_cleanup(&cm->workers[j].io_uring_ctx);
            }
            pthread_mutex_destroy(&cm->stats_mutex);
            free(cm->workers);
            return ret;
        }
        
        // Allocate operation queue
        worker->operations = calloc(queue_depth, sizeof(struct xcopy_operation));
        if (!worker->operations) {
            io_uring_nvme_cleanup(&worker->io_uring_ctx);
            for (uint32_t j = 0; j < i; j++) {
                io_uring_nvme_cleanup(&cm->workers[j].io_uring_ctx);
                free(cm->workers[j].operations);
            }
            pthread_mutex_destroy(&cm->stats_mutex);
            free(cm->workers);
            return -ENOMEM;
        }
        
        worker->queue_depth = queue_depth;
        worker->queue_head = 0;
        worker->queue_tail = 0;
        worker->in_flight = 0;
        
        // Initialize synchronization primitives
        if (pthread_mutex_init(&worker->queue_mutex, NULL) != 0) {
            free(worker->operations);
            io_uring_nvme_cleanup(&worker->io_uring_ctx);
            for (uint32_t j = 0; j < i; j++) {
                io_uring_nvme_cleanup(&cm->workers[j].io_uring_ctx);
                free(cm->workers[j].operations);
                pthread_mutex_destroy(&cm->workers[j].queue_mutex);
            }
            pthread_mutex_destroy(&cm->stats_mutex);
            free(cm->workers);
            return -1;
        }
        
        if (pthread_cond_init(&worker->queue_cond, NULL) != 0) {
            pthread_mutex_destroy(&worker->queue_mutex);
            free(worker->operations);
            io_uring_nvme_cleanup(&worker->io_uring_ctx);
            for (uint32_t j = 0; j < i; j++) {
                io_uring_nvme_cleanup(&cm->workers[j].io_uring_ctx);
                free(cm->workers[j].operations);
                pthread_mutex_destroy(&cm->workers[j].queue_mutex);
                pthread_cond_destroy(&cm->workers[j].queue_cond);
            }
            pthread_mutex_destroy(&cm->stats_mutex);
            free(cm->workers);
            return -1;
        }
        
        worker->min_latency_us = UINT64_MAX;
    }
    
    return 0;
}

int concurrency_manager_start(struct concurrency_manager *cm) {
    if (!cm) {
        return -EINVAL;
    }
    
    // Start worker threads
    for (uint32_t i = 0; i < cm->num_workers; i++) {
        struct worker_thread *worker = &cm->workers[i];
        
        if (pthread_create(&worker->thread, NULL, worker_thread_func, worker) != 0) {
            // Stop already started threads
            for (uint32_t j = 0; j < i; j++) {
                cm->workers[j].should_stop = true;
                pthread_join(cm->workers[j].thread, NULL);
            }
            return -1;
        }
    }
    
    return 0;
}

int concurrency_manager_submit(struct concurrency_manager *cm,
                              struct xcopy_operation *op) {
    if (!cm || !op) {
        return -EINVAL;
    }
    
    // Round-robin selection of worker thread
    static uint32_t next_worker = 0;
    uint32_t worker_idx = next_worker % cm->num_workers;
    next_worker++;
    
    struct worker_thread *worker = &cm->workers[worker_idx];
    
    // Wait for queue space
    pthread_mutex_lock(&worker->queue_mutex);
    while (worker->in_flight >= worker->queue_depth) {
        pthread_cond_wait(&worker->queue_cond, &worker->queue_mutex);
    }
    
    // Find available slot in operation queue
    uint32_t slot = worker->queue_tail;
    worker->queue_tail = (worker->queue_tail + 1) % worker->queue_depth;
    worker->in_flight++;
    
    pthread_mutex_unlock(&worker->queue_mutex);
    
    // Copy operation to queue slot
    struct xcopy_operation *queued_op = &worker->operations[slot];
    memcpy(queued_op, op, sizeof(*op));
    queued_op->user_data = worker;
    
    // Record start time
    struct timeval tv;
    gettimeofday(&tv, NULL);
    queued_op->start_time_us = (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    
    // Prepare command data (range descriptors)
    size_t data_size = xcopy_cmd_get_data_size(queued_op->num_ranges);
    void *data = queued_op->ranges;
    
    // Submit command via io_uring (async)
    int rc = io_uring_nvme_submit_passthru(&worker->io_uring_ctx,
                                            worker->nvme_ctx,
                                            queued_op->dst_nsid,
                                            &queued_op->cmd,
                                            data,
                                            data_size,
                                            xcopy_completion_cb,
                                            queued_op);
    
    if (rc != 0) {
        pthread_mutex_lock(&worker->queue_mutex);
        worker->in_flight--;
        pthread_cond_signal(&worker->queue_cond);
        pthread_mutex_unlock(&worker->queue_mutex);
        return rc;
    }
    
    return 0;
}

void concurrency_manager_stop(struct concurrency_manager *cm) {
    if (!cm) {
        return;
    }
    
    // Signal all workers to stop
    for (uint32_t i = 0; i < cm->num_workers; i++) {
        cm->workers[i].should_stop = true;
    }
}

void concurrency_manager_wait(struct concurrency_manager *cm) {
    if (!cm) {
        return;
    }
    
    // Wait for all worker threads
    for (uint32_t i = 0; i < cm->num_workers; i++) {
        if (cm->workers[i].thread) {
            pthread_join(cm->workers[i].thread, NULL);
        }
    }
    
    // Wait for all operations to complete
    for (uint32_t i = 0; i < cm->num_workers; i++) {
        struct worker_thread *worker = &cm->workers[i];
        while (worker->in_flight > 0) {
            io_uring_nvme_process_completions(&worker->io_uring_ctx, 32);
            usleep(100);
        }
    }
}

void concurrency_manager_get_stats(struct concurrency_manager *cm,
                                  struct xcopy_stats *stats) {
    if (!cm || !stats) {
        return;
    }
    
    memset(stats, 0, sizeof(*stats));
    
    pthread_mutex_lock(&cm->stats_mutex);
    
    for (uint32_t i = 0; i < cm->num_workers; i++) {
        struct worker_thread *worker = &cm->workers[i];
        
        stats->operations_completed += worker->ops_completed;
        stats->operations_failed += worker->ops_failed;
        stats->bytes_copied += worker->bytes_copied;
        stats->total_latency_us += worker->total_latency_us;
        
        if (worker->min_latency_us < stats->min_latency_us) {
            stats->min_latency_us = worker->min_latency_us;
        }
        
        if (worker->max_latency_us > stats->max_latency_us) {
            stats->max_latency_us = worker->max_latency_us;
        }
    }
    
    pthread_mutex_unlock(&cm->stats_mutex);
}

void concurrency_manager_cleanup(struct concurrency_manager *cm) {
    if (!cm) {
        return;
    }
    
    // Stop and wait for threads
    concurrency_manager_stop(cm);
    concurrency_manager_wait(cm);
    
    // Cleanup workers
    for (uint32_t i = 0; i < cm->num_workers; i++) {
        struct worker_thread *worker = &cm->workers[i];
        
        // Cleanup io_uring context
        io_uring_nvme_cleanup(&worker->io_uring_ctx);
        
        if (worker->operations) {
            free(worker->operations);
        }
        
        pthread_mutex_destroy(&worker->queue_mutex);
        pthread_cond_destroy(&worker->queue_cond);
    }
    
    if (cm->workers) {
        free(cm->workers);
    }
    
    pthread_mutex_destroy(&cm->stats_mutex);
    memset(cm, 0, sizeof(*cm));
}

