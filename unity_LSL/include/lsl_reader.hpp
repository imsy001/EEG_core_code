#pragma once
#include <atomic>
#include <thread>
#include <string>
#include <functional>
#include <vector>

#include "external/liblsl/include/lsl_cpp.h"
#include "log_buffer.hpp"

class LSLReader {
public:
    using OnMarkerText = std::function<void(const std::string& text, double lsl_ts)>;

    explicit LSLReader(std::string stream_name = "UnityMarkers");
    ~LSLReader();

    // NEW: callback added
    void start(LogBuffer& log, OnMarkerText cb);
    void stop();
    bool running() const { return running_; }

private:
    void thread_main();

    std::string stream_name_;
    std::atomic<bool> running_{ false };
    std::thread th_;

    LogBuffer* log_ = nullptr;
    OnMarkerText cb_; // ✅ NEW
};
