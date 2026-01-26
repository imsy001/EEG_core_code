#pragma once

#include <cstddef>
#include <cstdint>

namespace config {

    // =========================
    // General system
    // =========================

    // Enable debug logging / overlays
    inline constexpr bool kDebugMode = true;

    // =========================
    // EEG device defaults
    // =========================
    // Used if device does not report metadata
    inline constexpr int kDefaultSampleRateHz = 500;
    inline constexpr int kDefaultNumChannels  = 32;

    // =========================
    // Buffering
    // =========================

    // Ring buffer length for live display (seconds)
    inline constexpr double kRingBufferSeconds = 10.0;

    // =========================
    // Epoch safety limits
    // =========================

    inline constexpr double kMinEpochSeconds = 0.1;
    inline constexpr double kMaxEpochSeconds = 10.0;

    // =========================
    // Experiment logic
    // =========================

    // Marker-based epoching (SPACE_DOWN / SPACE_UP)
    inline constexpr bool kUseMarkerEpoching = true;

    // =========================
    // Inference
    // =========================

    // Run inference online during experiment
    inline constexpr bool kEnableOnlineInference = true;

    // Default ONNX model path
    inline constexpr const char* kOnnxModelPath =
        "models/eeg_classifier.onnx";

    // =========================
    // LSL
    // =========================

    // Semantic stream names
    inline constexpr const char* kLSLMarkerStreamName  = "EEG_MARKERS";
    inline constexpr const char* kLSLControlStreamName = "EEG_CONTROL";

    // =========================
    // Timing policy
    // =========================

    // Prefer device-provided timestamps over host time
    inline constexpr bool kPreferDeviceTimestamp = false;

} // namespace config
