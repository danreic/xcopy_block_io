#ifndef ERROR_HANDLER_H
#define ERROR_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include <atomic>
#include <unordered_map>
#include <mutex>

namespace xload {

// NVMe status code definitions
#define NVME_SC_SUCCESS 0x0000
#define NVME_SC_QUEUE_FULL 0x0807
#define NVME_SC_INSUFFICIENT_RESOURCES 0x0189

class ErrorHandler {
public:
    // Check if error is a saturation/backpressure error
    static bool is_saturation_error(uint16_t status_code);
    
    // Check if error is fatal
    static bool is_fatal_error(uint16_t status_code);
    
    // Get error description
    static const char* get_error_string(uint16_t status_code);
    
    // Error counter
    class ErrorCounter {
    public:
        ErrorCounter();
        void record_error(uint16_t status_code);
        uint64_t get_count(uint16_t status_code) const;
        uint64_t get_total_errors() const;
        void reset();
        void get_all_errors(std::unordered_map<uint16_t, uint64_t>& errors) const;
        
    private:
        std::atomic<uint64_t> saturation_errors_;
        std::atomic<uint64_t> fatal_errors_;
        std::atomic<uint64_t> other_errors_;
        std::unordered_map<uint16_t, std::atomic<uint64_t>> error_counts_;
        mutable std::mutex mutex_;
    };
};

} // namespace xload

#endif // ERROR_HANDLER_H

