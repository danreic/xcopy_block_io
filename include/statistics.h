#ifndef STATISTICS_H
#define STATISTICS_H

#include <stdint.h>
#include <stdbool.h>
#include "xcopy_tool.h"

// Statistics collector
struct statistics_collector {
    struct xcopy_stats stats;
    pthread_mutex_t mutex;
    bool enabled;
};

// Initialize statistics collector
int statistics_init(struct statistics_collector *collector);

// Record a completed operation
void statistics_record_completion(struct statistics_collector *collector,
                                 uint64_t bytes,
                                 uint64_t latency_us);

// Record a failed operation
void statistics_record_failure(struct statistics_collector *collector);

// Get current statistics
void statistics_get(struct statistics_collector *collector,
                   struct xcopy_stats *stats);

// Reset statistics
void statistics_reset(struct statistics_collector *collector);

// Print statistics
void statistics_print(struct statistics_collector *collector);

// Calculate throughput
double statistics_calculate_throughput(struct xcopy_stats *stats, double elapsed_sec);

// Cleanup statistics collector
void statistics_cleanup(struct statistics_collector *collector);

#endif // STATISTICS_H

