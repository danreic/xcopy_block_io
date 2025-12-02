#include "error_handler.h"
#include <mutex>
#include <cstring>

namespace xload {

bool ErrorHandler::is_saturation_error(uint16_t status_code) {
    // Extract status code (lower 8 bits)
    uint8_t sc = status_code & 0xFF;
    
    // Check for known saturation errors
    if (sc == 0x07) { // Queue Full
        return true;
    }
    if (sc == 0x89) { // Insufficient Resources
        return true;
    }
    
    return false;
}

bool ErrorHandler::is_fatal_error(uint16_t status_code) {
    // Extract status code type (bits 9-11)
    uint8_t sct = (status_code >> 9) & 0x7;
    
    // Fatal errors are type 2 (Command Specific Status) or type 3 (Media and Data Integrity Errors)
    // or type 4 (Path Related Status) with certain codes
    if (sct >= 2) {
        return true;
    }
    
    // Check for specific fatal status codes
    uint8_t sc = status_code & 0xFF;
    if (sc == 0x01) { // Invalid Command Opcode
        return true;
    }
    if (sc == 0x02) { // Invalid Field in Command
        return true;
    }
    
    return false;
}

const char* ErrorHandler::get_error_string(uint16_t status_code) {
    uint8_t sc = status_code & 0xFF;
    
    switch (sc) {
        case 0x00: return "Success";
        case 0x01: return "Invalid Command Opcode";
        case 0x02: return "Invalid Field in Command";
        case 0x07: return "Queue Full";
        case 0x89: return "Insufficient Resources";
        default: return "Unknown Error";
    }
}

ErrorHandler::ErrorCounter::ErrorCounter()
    : saturation_errors_(0)
    , fatal_errors_(0)
    , other_errors_(0)
{
}

void ErrorHandler::ErrorCounter::record_error(uint16_t status_code) {
    if (is_saturation_error(status_code)) {
        saturation_errors_++;
    } else if (is_fatal_error(status_code)) {
        fatal_errors_++;
    } else {
        other_errors_++;
    }
    
    // Track individual error codes
    std::lock_guard<std::mutex> lock(mutex_);
    error_counts_[status_code]++;
}

uint64_t ErrorHandler::ErrorCounter::get_count(uint16_t status_code) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = error_counts_.find(status_code);
    if (it != error_counts_.end()) {
        return it->second.load();
    }
    return 0;
}

uint64_t ErrorHandler::ErrorCounter::get_total_errors() const {
    return saturation_errors_.load() + fatal_errors_.load() + other_errors_.load();
}

void ErrorHandler::ErrorCounter::reset() {
    saturation_errors_ = 0;
    fatal_errors_ = 0;
    other_errors_ = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    error_counts_.clear();
}

void ErrorHandler::ErrorCounter::get_all_errors(std::unordered_map<uint16_t, uint64_t>& errors) const {
    std::lock_guard<std::mutex> lock(mutex_);
    errors.clear();
    for (const auto& pair : error_counts_) {
        errors[pair.first] = pair.second.load();
    }
}

} // namespace xload

