#ifndef HIGH_RES_TIMER_H
#define HIGH_RES_TIMER_H

#include <stdint.h>
#include <time.h>

namespace xload {

class HighResTimer {
public:
    // Get current timestamp in nanoseconds
    static uint64_t now_ns();
    
    // Get current timestamp in microseconds
    static uint64_t now_us();
    
    // Get CPU cycle counter (if available)
    static uint64_t cycles();
    
    // Convert nanoseconds to microseconds
    static double ns_to_us(uint64_t ns);
    
    // Convert nanoseconds to milliseconds
    static double ns_to_ms(uint64_t ns);
    
    // Get CPU frequency (for cycle counter conversion)
    static double get_cpu_freq_ghz();
    
private:
    static double cpu_freq_ghz_;
    static bool cpu_freq_initialized_;
    static void init_cpu_freq();
};

} // namespace xload

#endif // HIGH_RES_TIMER_H

