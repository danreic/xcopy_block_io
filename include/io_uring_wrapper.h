#ifndef IO_URING_WRAPPER_H
#define IO_URING_WRAPPER_H

#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <fcntl.h>
#include <liburing.h>
#include <sys/types.h>

// Forward declaration
struct nvme_context;
struct nvme_passthru_cmd;

// Completion callback type
typedef void (*io_uring_completion_cb)(void *user_data, int result, uint32_t status);

// io_uring context for async NVMe operations
struct io_uring_nvme_ctx {
    struct io_uring ring;
    bool initialized;
    uint32_t queue_depth;
};

// Initialize io_uring context
int io_uring_nvme_init(struct io_uring_nvme_ctx *ctx, uint32_t queue_depth);

// Submit async passthrough command
int io_uring_nvme_submit_passthru(struct io_uring_nvme_ctx *ctx,
                                   struct nvme_context *nvme_ctx,
                                   uint32_t nsid,
                                   struct nvme_passthru_cmd *cmd,
                                   void *data,
                                   size_t data_len,
                                   io_uring_completion_cb cb,
                                   void *user_data);

// Process completions (non-blocking)
int io_uring_nvme_process_completions(struct io_uring_nvme_ctx *ctx, uint32_t max_completions);

// Wait for completions (blocking)
int io_uring_nvme_wait_completions(struct io_uring_nvme_ctx *ctx, uint32_t max_completions);

// Cleanup io_uring context
void io_uring_nvme_cleanup(struct io_uring_nvme_ctx *ctx);

#endif // IO_URING_WRAPPER_H

