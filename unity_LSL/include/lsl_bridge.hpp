#pragma once

#include "core/include/types.hpp"

// ===================================
// LSLBridge
// ===================================
// - Transport abstraction for LSL
// - Sends semantic events only
// - NO Unity / UI / experiment logic
// - Implementation lives in unity_LSL.cpp (or others)
// ===================================

class LSLBridge {
public:
    virtual ~LSLBridge() = default;

    // =========================
    // Marker channel
    // =========================
    // Used for experiment timing & epoching
    virtual void send_marker(Marker marker,
                             double timestamp_sec) = 0;

    // =========================
    // Control / command channel
    // =========================
    // Used for semantic control (e.g., Direction)
    virtual void send_direction(Direction direction,
                                double timestamp_sec) = 0;

};
