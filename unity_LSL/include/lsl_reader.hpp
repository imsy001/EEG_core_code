#pragma once
#include <atomic>
#include <thread>
#include <string>
#include "lsl_cpp.h"

#include "log_buffer.hpp"

class LSLReader {
public:
    explicit LSLReader(std::string stream_name = "UnityMarkers");
    ~LSLReader();

    void start(LogBuffer& log);
    void stop();
    bool running() const { return running_; }

private:
    void thread_main();

    std::string stream_name_;
    std::atomic<bool> running_{ false };
    std::thread th_;
    LogBuffer* log_ = nullptr;
};
