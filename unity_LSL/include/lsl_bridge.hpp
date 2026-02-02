#pragma once

#include <functional>
#include <string>          // ✅ add
#include "core/include/types.hpp"

class LSLBridge {
public:
    using OnMarkerText = std::function<void(const std::string&, double)>;

    virtual ~LSLBridge() = default;

    // TX
    virtual void send_marker(Marker marker, double timestamp_sec) = 0;
    virtual void send_direction(Direction direction, double timestamp_sec) = 0;
    virtual void send_marker_text(const std::string& text, double timestamp_sec) = 0;

    // RX
    virtual void start_rx(OnMarkerText cb) = 0;
    virtual void stop_rx() = 0;
    virtual bool rx_running() const = 0;
};
