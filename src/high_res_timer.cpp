#include "high_res_timer.h"
#include <unistd.h>
#include <cstdio>
#include <cstring>

namespace xload {

double HighResTimer::cpu_freq_ghz_ = 0.0;
bool HighResTimer::cpu_freq_initialized_ = false;

uint64_t HighResTimer::now_ns() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }
    // Fallback to CLOCK_MONOTONIC if RAW is not available
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }
    return 0;
}

uint64_t HighResTimer::now_us() {
    return now_ns() / 1000;
}

uint64_t HighResTimer::cycles() {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    // Fallback to nanosecond timer
    return now_ns();
#endif
}

double HighResTimer::ns_to_us(uint64_t ns) {
    return (double)ns / 1000.0;
}

double HighResTimer::ns_to_ms(uint64_t ns) {
    return (double)ns / 1000000.0;
}

void HighResTimer::init_cpu_freq() {
    if (cpu_freq_initialized_) {
        return;
    }
    
    // Try to read CPU frequency from /proc/cpuinfo
    FILE* fp = fopen("/proc/cpuinfo", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "cpu MHz", 7) == 0) {
                double mhz = 0.0;
                if (sscanf(line, "cpu MHz : %lf", &mhz) == 1) {
                    cpu_freq_ghz_ = mhz / 1000.0;
                    cpu_freq_initialized_ = true;
                    fclose(fp);
                    return;
                }
            }
        }
        fclose(fp);
    }
    
    // Fallback: measure CPU frequency by timing rdtsc
#if defined(__x86_64__) || defined(__i386__)
    uint64_t start_cycles = cycles();
    uint64_t start_ns = now_ns();
    usleep(100000); // Sleep 100ms
    uint64_t end_cycles = cycles();
    uint64_t end_ns = now_ns();
    
    if (end_ns > start_ns && end_cycles > start_cycles) {
        uint64_t cycles_diff = end_cycles - start_cycles;
        uint64_t ns_diff = end_ns - start_ns;
        cpu_freq_ghz_ = (double)cycles_diff / (double)ns_diff;
        cpu_freq_initialized_ = true;
    }
#endif
    
    // Default fallback
    if (!cpu_freq_initialized_) {
        cpu_freq_ghz_ = 2.0; // Assume 2 GHz
        cpu_freq_initialized_ = true;
    }
}

double HighResTimer::get_cpu_freq_ghz() {
    if (!cpu_freq_initialized_) {
        init_cpu_freq();
    }
    return cpu_freq_ghz_;
}

} // namespace xload

