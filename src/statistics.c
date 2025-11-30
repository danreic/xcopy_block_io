#include "statistics.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <stdint.h>
#include <limits.h>

int statistics_init(struct statistics_collector *collector) {
    if (!collector) {
        return -1;
    }
    
    memset(collector, 0, sizeof(*collector));
    
    if (pthread_mutex_init(&collector->mutex, NULL) != 0) {
        return -1;
    }
    
    collector->enabled = true;
    collector->stats.min_latency_us = UINT64_MAX;
    
    return 0;
}

void statistics_record_completion(struct statistics_collector *collector,
                                 uint64_t bytes,
                                 uint64_t latency_us) {
    if (!collector || !collector->enabled) {
        return;
    }
    
    pthread_mutex_lock(&collector->mutex);
    
    collector->stats.operations_completed++;
    collector->stats.bytes_copied += bytes;
    collector->stats.total_latency_us += latency_us;
    
    if (latency_us < collector->stats.min_latency_us) {
        collector->stats.min_latency_us = latency_us;
    }
    
    if (latency_us > collector->stats.max_latency_us) {
        collector->stats.max_latency_us = latency_us;
    }
    
    pthread_mutex_unlock(&collector->mutex);
}

void statistics_record_failure(struct statistics_collector *collector) {
    if (!collector || !collector->enabled) {
        return;
    }
    
    pthread_mutex_lock(&collector->mutex);
    collector->stats.operations_failed++;
    pthread_mutex_unlock(&collector->mutex);
}

void statistics_get(struct statistics_collector *collector,
                   struct xcopy_stats *stats) {
    if (!collector || !stats) {
        return;
    }
    
    pthread_mutex_lock(&collector->mutex);
    memcpy(stats, &collector->stats, sizeof(*stats));
    pthread_mutex_unlock(&collector->mutex);
}

void statistics_reset(struct statistics_collector *collector) {
    if (!collector) {
        return;
    }
    
    pthread_mutex_lock(&collector->mutex);
    memset(&collector->stats, 0, sizeof(collector->stats));
    collector->stats.min_latency_us = UINT64_MAX;
    pthread_mutex_unlock(&collector->mutex);
}

double statistics_calculate_throughput(struct xcopy_stats *stats, double elapsed_sec) {
    if (!stats || elapsed_sec <= 0) {
        return 0.0;
    }
    
    // Convert bytes to MB and calculate throughput
    double bytes_mb = (double)stats->bytes_copied / (1024.0 * 1024.0);
    return bytes_mb / elapsed_sec;
}

void statistics_print(struct statistics_collector *collector) {
    if (!collector) {
        return;
    }
    
    struct xcopy_stats stats;
    statistics_get(collector, &stats);
    
    printf("\n=== Statistics ===\n");
    printf("Operations completed: %llu\n", (unsigned long long)stats.operations_completed);
    printf("Operations failed: %llu\n", (unsigned long long)stats.operations_failed);
    printf("Bytes copied: %llu (%.2f MB)\n", (unsigned long long)stats.bytes_copied,
           (double)stats.bytes_copied / (1024.0 * 1024.0));
    
    if (stats.operations_completed > 0) {
        double avg_latency_us = (double)stats.total_latency_us / stats.operations_completed;
        printf("Average latency: %.2f us\n", avg_latency_us);
        printf("Min latency: %llu us\n", (unsigned long long)stats.min_latency_us);
        printf("Max latency: %llu us\n", (unsigned long long)stats.max_latency_us);
    }
    
    if (stats.throughput_mbps > 0) {
        printf("Throughput: %.2f MB/s\n", stats.throughput_mbps);
    }
    
    printf("==================\n");
}

void statistics_cleanup(struct statistics_collector *collector) {
    if (!collector) {
        return;
    }
    
    pthread_mutex_destroy(&collector->mutex);
    memset(collector, 0, sizeof(*collector));
}

