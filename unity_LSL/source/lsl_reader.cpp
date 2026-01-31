#include "unity_LSL/include/lsl_reader.hpp"
//#include "lsl_cpp.h"

#include <sstream>
#include <iomanip>

LSLReader::LSLReader(std::string stream_name)
    : stream_name_(std::move(stream_name)) {
}

LSLReader::~LSLReader() {
    stop();
}

void LSLReader::start(LogBuffer& log) {
    if (running_) return;

    log_ = &log;
    running_ = true;

    th_ = std::thread(&LSLReader::thread_main, this);
}

void LSLReader::stop() {
    if (!running_) return;

    running_ = false;
    if (th_.joinable())
        th_.join();
}

void LSLReader::thread_main() {
    try {
        log_->push("[LSL] resolving stream: " + stream_name_);

        auto results = lsl::resolve_stream("name", stream_name_, 1, 5.0);
        if (results.empty()) {
            log_->push("[LSL] ERROR: stream not found");
            running_ = false;
            return;
        }

        lsl::stream_inlet inlet(results[0]);
        log_->push("[LSL] connected");

        std::vector<std::string> sample(1);

        while (running_) {
            double lsl_ts = inlet.pull_sample(sample, 0.2);
            if (lsl_ts == 0.0) continue;

            std::ostringstream oss;
            oss << std::fixed << std::setprecision(6)
                << "lsl_ts=" << lsl_ts << " | " << sample[0];

            log_->push(oss.str());
        }

        log_->push("[LSL] stopped");
    }
    catch (const std::exception& e) {
        log_->push(std::string("[LSL] EXCEPTION: ") + e.what());
        running_ = false;
    }
}
