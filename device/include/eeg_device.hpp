#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/include/types.hpp"   // EEGSample, DeviceInfo

class EEGDevice {
public:
    using SampleCallback = std::function<void(const EEGSample&)>;

    virtual ~EEGDevice() = default;

    // Connect/configure device resources (optional; can be no-op)
    virtual bool open() = 0;
    virtual void close() = 0;

    // Start/stop streaming. start() must be non-blocking.
    virtual bool start_streaming(SampleCallback cb) = 0;
    virtual void stop_streaming() = 0;

    // Metadata
    virtual DeviceInfo info() const = 0;

    // Optional status helpers
    virtual bool is_open() const = 0;
    virtual bool is_streaming() const = 0;

    // Human-readable last error for debugging (no exceptions in real-time path)
    virtual std::string last_error() const = 0;
};
