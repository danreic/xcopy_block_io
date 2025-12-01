#include "io_uring_wrapper.h"
#include "nvme_wrapper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

// Static registry to map ctx pointers to extended contexts
#define MAX_CTX_REGISTRY 16
static struct io_uring_nvme_ctx_ext *ctx_registry[MAX_CTX_REGISTRY];
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;

// Internal structure to track pending operations
struct pending_op {
    struct nvme_passthru_cmd cmd;
    void *data;
    size_t data_len;
    io_uring_completion_cb cb;
    void *user_data;
    uint32_t nsid;
    struct nvme_context *nvme_ctx;
    atomic_bool completed;
};

// Extended context with thread pool
struct io_uring_nvme_ctx_ext {
    struct io_uring ring;
    struct io_uring_nvme_ctx *ctx;  // Pointer back to the public ctx for lookup
    bool initialized;
    uint32_t queue_depth;
    int registry_index;  // Index in registry
    
    // Thread pool for executing synchronous libnvme calls
    pthread_t *worker_threads;
    uint32_t num_workers;
    atomic_bool should_stop;
    
    // Work queue
    struct pending_op **work_queue;
    uint32_t queue_size;
    uint32_t queue_head;
    uint32_t queue_tail;
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
    pthread_cond_t queue_not_full;
};

// Worker thread function
static void *worker_thread_func(void *arg) {
    struct io_uring_nvme_ctx_ext *ctx = (struct io_uring_nvme_ctx_ext *)arg;
    
    while (!atomic_load(&ctx->should_stop)) {
        struct pending_op *op = NULL;
        
        // Get work from queue
        pthread_mutex_lock(&ctx->queue_mutex);
        while (ctx->queue_head == ctx->queue_tail && !atomic_load(&ctx->should_stop)) {
            pthread_cond_wait(&ctx->queue_cond, &ctx->queue_mutex);
        }
        
        if (atomic_load(&ctx->should_stop)) {
            pthread_mutex_unlock(&ctx->queue_mutex);
            break;
        }
        
        // Get operation from queue
        op = ctx->work_queue[ctx->queue_tail];
        ctx->queue_tail = (ctx->queue_tail + 1) % ctx->queue_size;
        pthread_cond_signal(&ctx->queue_not_full);
        pthread_mutex_unlock(&ctx->queue_mutex);
        
        if (op) {
            // Verify context is valid before using it
            if (!op->nvme_ctx) {
                fprintf(stderr, "ERROR: worker_thread: op->nvme_ctx is NULL\n");
                if (op->cb) {
                    op->cb(op->user_data, -EINVAL, EINVAL);
                }
                atomic_store(&op->completed, true);
                free(op);
                continue;
            }
            
            // Debug: Check context state (only print once)
            static int ctx_debug_count = 0;
            if (ctx_debug_count == 0) {
                fprintf(stderr, "DEBUG: worker_thread: Using nvme_ctx=%p, connected=%d, ctrl_fd=%d, initialized=%d\n",
                        op->nvme_ctx, op->nvme_ctx ? op->nvme_ctx->connected : -1, 
                        op->nvme_ctx ? op->nvme_ctx->ctrl_fd : -1, 
                        op->nvme_ctx ? op->nvme_ctx->initialized : -1);
                ctx_debug_count++;
            }
            
            // Execute the synchronous libnvme call
            int result = nvme_wrapper_submit_passthru(op->nvme_ctx,
                                                       op->nsid,
                                                       &op->cmd,
                                                       op->data,
                                                       op->data_len);
            
            // Call completion callback
            if (op->cb) {
                // result is negative on error, positive or zero on success
                // status should be the NVMe status code
                uint32_t status = (result < 0) ? (uint32_t)-result : 0;
                op->cb(op->user_data, result, status);
            }
            
            atomic_store(&op->completed, true);
            free(op);
        }
    }
    
    return NULL;
}

int io_uring_nvme_init(struct io_uring_nvme_ctx *ctx, uint32_t queue_depth) {
    if (!ctx) {
        return -EINVAL;
    }
    
    // Allocate extended context
    struct io_uring_nvme_ctx_ext *ext = calloc(1, sizeof(struct io_uring_nvme_ctx_ext));
    if (!ext) {
        return -ENOMEM;
    }
    
    ext->queue_depth = queue_depth;
    ext->queue_size = queue_depth * 2;  // Allow some headroom
    ext->num_workers = 4;  // Default to 4 worker threads
    ext->registry_index = -1;
    
    // Initialize io_uring
    int ret = io_uring_queue_init(queue_depth, &ext->ring, 0);
    if (ret < 0) {
        fprintf(stderr, "Error: Failed to initialize io_uring: %s\n", strerror(-ret));
        free(ext);
        return ret;
    }
    
    // Allocate work queue
    ext->work_queue = calloc(ext->queue_size, sizeof(struct pending_op *));
    if (!ext->work_queue) {
        io_uring_queue_exit(&ext->ring);
        free(ext);
        return -ENOMEM;
    }
    
    // Initialize synchronization primitives
    pthread_mutex_init(&ext->queue_mutex, NULL);
    pthread_cond_init(&ext->queue_cond, NULL);
    pthread_cond_init(&ext->queue_not_full, NULL);
    
    // Create worker threads
    ext->worker_threads = calloc(ext->num_workers, sizeof(pthread_t));
    if (!ext->worker_threads) {
        pthread_cond_destroy(&ext->queue_not_full);
        pthread_cond_destroy(&ext->queue_cond);
        pthread_mutex_destroy(&ext->queue_mutex);
        free(ext->work_queue);
        io_uring_queue_exit(&ext->ring);
        free(ext);
        return -ENOMEM;
    }
    
    atomic_store(&ext->should_stop, false);
    
    for (uint32_t i = 0; i < ext->num_workers; i++) {
        if (pthread_create(&ext->worker_threads[i], NULL, worker_thread_func, ext) != 0) {
            // Cleanup on failure
            atomic_store(&ext->should_stop, true);
            pthread_cond_broadcast(&ext->queue_cond);
            for (uint32_t j = 0; j < i; j++) {
                pthread_join(ext->worker_threads[j], NULL);
            }
            free(ext->worker_threads);
            pthread_cond_destroy(&ext->queue_not_full);
            pthread_cond_destroy(&ext->queue_cond);
            pthread_mutex_destroy(&ext->queue_mutex);
            free(ext->work_queue);
            io_uring_queue_exit(&ext->ring);
            free(ext);
            return -1;
        }
    }
    
    ext->initialized = true;
    
    // Copy ring to ctx
    memcpy(&ctx->ring, &ext->ring, sizeof(struct io_uring));
    ctx->initialized = true;
    ctx->queue_depth = queue_depth;
    
    // Store ctx pointer in ext for lookup
    ext->ctx = ctx;
    
    // Store extended context in registry
    pthread_mutex_lock(&registry_mutex);
    int registry_idx = -1;
    for (int i = 0; i < MAX_CTX_REGISTRY; i++) {
        if (ctx_registry[i] == NULL) {
            ctx_registry[i] = ext;
            ext->registry_index = i;
            registry_idx = i;
            break;
        }
    }
    pthread_mutex_unlock(&registry_mutex);
    
    if (registry_idx < 0) {
        // Registry full - cleanup and return error
        pthread_cond_destroy(&ext->queue_not_full);
        pthread_cond_destroy(&ext->queue_cond);
        pthread_mutex_destroy(&ext->queue_mutex);
        free(ext->worker_threads);
        free(ext->work_queue);
        io_uring_queue_exit(&ext->ring);
        free(ext);
        return -ENOMEM;
    }
    
    return 0;
}

// Helper to get extended context
static struct io_uring_nvme_ctx_ext *get_ext_ctx(struct io_uring_nvme_ctx *ctx) {
    // Look up in registry by ctx pointer (stored in ext->ctx)
    pthread_mutex_lock(&registry_mutex);
    for (int i = 0; i < MAX_CTX_REGISTRY; i++) {
        if (ctx_registry[i] != NULL && ctx_registry[i]->ctx == ctx) {
            struct io_uring_nvme_ctx_ext *ext = ctx_registry[i];
            pthread_mutex_unlock(&registry_mutex);
            return ext;
        }
    }
    pthread_mutex_unlock(&registry_mutex);
    return NULL;
}

int io_uring_nvme_submit_passthru(struct io_uring_nvme_ctx *ctx,
                                   struct nvme_context *nvme_ctx,
                                   uint32_t nsid,
                                   struct nvme_passthru_cmd *cmd,
                                   void *data,
                                   size_t data_len,
                                   io_uring_completion_cb cb,
                                   void *user_data) {
    // Debug: Check parameters (only once)
    static int param_debug_logged = 0;
    if (!param_debug_logged) {
        fprintf(stderr, "DEBUG: io_uring_nvme_submit_passthru: ctx=%p, initialized=%d, nvme_ctx=%p, cmd=%p, nsid=%u\n",
                ctx, ctx ? ctx->initialized : 0, nvme_ctx, cmd, nsid);
        param_debug_logged = 1;
    }
    
    if (!ctx || !ctx->initialized || !nvme_ctx || !cmd) {
        static int invalid_param_logged = 0;
        if (!invalid_param_logged) {
            fprintf(stderr, "ERROR: io_uring_nvme_submit_passthru: Invalid parameters (ctx=%p, initialized=%d, nvme_ctx=%p, cmd=%p)\n",
                    ctx, ctx ? ctx->initialized : 0, nvme_ctx, cmd);
            invalid_param_logged = 1;
        }
        return -EINVAL;
    }
    
    struct io_uring_nvme_ctx_ext *ext = get_ext_ctx(ctx);
    if (!ext) {
        static int ext_not_found_logged = 0;
        if (!ext_not_found_logged) {
            fprintf(stderr, "ERROR: io_uring_nvme_submit_passthru: get_ext_ctx returned NULL\n");
            ext_not_found_logged = 1;
        }
        return -EINVAL;
    }
    
    // Allocate pending operation
    struct pending_op *op = malloc(sizeof(struct pending_op));
    if (!op) {
        return -ENOMEM;
    }
    
    memcpy(&op->cmd, cmd, sizeof(*cmd));
    op->nsid = nsid;
    op->nvme_ctx = nvme_ctx;
    op->data = data;
    op->data_len = data_len;
    op->cb = cb;
    op->user_data = user_data;
    atomic_store(&op->completed, false);
    
    // Debug: Verify context is connected when storing it
    static int debug_count = 0;
    if (debug_count < 3 && (!op->nvme_ctx || !op->nvme_ctx->connected)) {
        fprintf(stderr, "WARNING: io_uring_nvme_submit_passthru: Storing unconnected context (ctx=%p, connected=%d)\n",
                op->nvme_ctx, op->nvme_ctx ? op->nvme_ctx->connected : 0);
        debug_count++;
    }
    
    // Add to work queue
    pthread_mutex_lock(&ext->queue_mutex);
    
    // Wait for queue space
    while (((ext->queue_head + 1) % ext->queue_size) == ext->queue_tail) {
        pthread_cond_wait(&ext->queue_not_full, &ext->queue_mutex);
    }
    
    ext->work_queue[ext->queue_head] = op;
    ext->queue_head = (ext->queue_head + 1) % ext->queue_size;
    
    pthread_cond_signal(&ext->queue_cond);
    pthread_mutex_unlock(&ext->queue_mutex);
    
    return 0;
}

int io_uring_nvme_process_completions(struct io_uring_nvme_ctx *ctx, uint32_t max_completions) {
    // Completions are handled by worker threads directly
    // This function is kept for API compatibility but doesn't need to do anything
    // since callbacks are called from worker threads
    (void)ctx;
    (void)max_completions;
    return 0;
}

int io_uring_nvme_wait_completions(struct io_uring_nvme_ctx *ctx, uint32_t max_completions) {
    // Same as process_completions - callbacks are handled by worker threads
    (void)ctx;
    (void)max_completions;
    return 0;
}

void io_uring_nvme_cleanup(struct io_uring_nvme_ctx *ctx) {
    if (!ctx || !ctx->initialized) {
        return;
    }
    
    struct io_uring_nvme_ctx_ext *ext = get_ext_ctx(ctx);
    if (!ext) {
        return;
    }
    
    // Stop worker threads
    atomic_store(&ext->should_stop, true);
    pthread_cond_broadcast(&ext->queue_cond);
    
    // Wait for threads to finish
    for (uint32_t i = 0; i < ext->num_workers; i++) {
        pthread_join(ext->worker_threads[i], NULL);
    }
    
    // Remove from registry
    pthread_mutex_lock(&registry_mutex);
    if (ext->registry_index >= 0 && ext->registry_index < MAX_CTX_REGISTRY) {
        ctx_registry[ext->registry_index] = NULL;
    }
    pthread_mutex_unlock(&registry_mutex);
    
    // Cleanup
    free(ext->worker_threads);
    pthread_cond_destroy(&ext->queue_not_full);
    pthread_cond_destroy(&ext->queue_cond);
    pthread_mutex_destroy(&ext->queue_mutex);
    free(ext->work_queue);
    io_uring_queue_exit(&ext->ring);
    free(ext);
    
    ctx->initialized = false;
}
