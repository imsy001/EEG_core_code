#pragma once
#include <atomic>
#include <thread>
#include <string>

#include "unity_LSL/include/lsl_bridge.hpp" 
#include "lsl_cpp.h"

class LSLBridgeImpl final : public LSLBridge {
public:
    LSLBridgeImpl(std::string rx_stream_name = "UnityMarkers",
        std::string tx_marker_stream_name = "CppToUnityMarkers",
        std::string tx_direction_stream_name = "CppToUnityDirections");

    ~LSLBridgeImpl() override;

    // TX
    void send_marker(Marker marker, double timestamp_sec) override;
    void send_direction(Direction direction, double timestamp_sec) override;
    void send_marker_text(const std::string& text, double timestamp_sec) override;

    // RX
    void start_rx(OnMarkerText cb) override;
    void stop_rx() override;
    bool rx_running() const override;

private:
    void rx_thread_main_();

    static const char* marker_to_text_(Marker m);
    static const char* direction_to_text_(Direction d);

private:
    std::string rx_stream_name_;

    lsl::stream_outlet tx_marker_outlet_;
    lsl::stream_outlet tx_direction_outlet_;

    std::atomic<bool> rx_running_{ false };
    std::thread rx_th_;
    OnMarkerText rx_cb_;
};
