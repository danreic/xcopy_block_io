#ifndef JSON_REPORTER_H
#define JSON_REPORTER_H

#include "statistics.h"
#include "config_manager.h"
#include <string>
#include <iostream>

namespace xload {

class JsonReporter {
public:
    // Generate JSON report from statistics
    static void generate_report(const Statistics& stats, 
                               const Config& config,
                               double elapsed_sec,
                               ::std::ostream& out);
    
    // Generate human-readable report
    static void generate_human_report(const Statistics& stats,
                                     const Config& config,
                                     double elapsed_sec,
                                     ::std::ostream& out);
};

} // namespace xload

#endif // JSON_REPORTER_H

