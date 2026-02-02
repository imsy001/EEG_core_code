#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

// ================================
// Marker (experiment timing)
// ================================
// Discrete events used for epoching
// e.g., SPACE_DOWN / SPACE_UP
enum class Marker : std::uint8_t {
    SPACE_DOWN = 0,
    SPACE_UP   = 1,
};

// ================================
// Direction (semantic control)
// ================================
enum class Direction : std::uint8_t {
    Left  = 0,
    Right = 1,
    Up    = 2,
    Down  = 3,
    ZOOM_IN = 4,
    ZOOM_OUT = 5
};


struct EpochMeta {
    double trigger_ts = 0.0;          // your timeline timestamp at trigger
    Marker marker = Marker::SPACE_DOWN;
    std::optional<Direction> direction; // optional
};


// ================================
// EEG sample (atomic unit)
// ================================
// One timestamped EEG sample across all channels
struct EEGSample {
    // Timestamp in seconds
    // Can be device time or host time (policy defined elsewhere)
    double timestamp_sec = 0.0;
    double lsl_ts = 0.0;      // device / LSL timestamp
    double system_ts = 0.0;  // std::chrono 기반

    // channels[c] = amplitude of channel c at this timestamp
    std::vector<float> channels;
};

// ================================
// Device metadata
// ================================
struct DeviceInfo {
    std::string name;
    int sample_rate_hz = 0;
    int num_channels   = 0;
};
