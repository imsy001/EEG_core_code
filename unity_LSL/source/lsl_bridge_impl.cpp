#include "unity_LSL/include/lsl_bridge_impl.hpp"

#include <vector>
#include <utility>

static lsl::stream_outlet make_string_outlet(const std::string& name,
    const std::string& type,
    const std::string& source_id) {
    lsl::stream_info info(
        name, type,
        1, 0,
        lsl::cf_string,
        source_id
    );
    return lsl::stream_outlet(info);
}

LSLBridgeImpl::LSLBridgeImpl(std::string rx_stream_name,
    std::string tx_marker_stream_name,
    std::string tx_direction_stream_name)
    : rx_stream_name_(std::move(rx_stream_name)),
    tx_marker_outlet_(make_string_outlet(tx_marker_stream_name, "Markers", "cpp_marker_sender")),
    tx_direction_outlet_(make_string_outlet(tx_direction_stream_name, "Directions", "cpp_direction_sender")) {
}

LSLBridgeImpl::~LSLBridgeImpl() {
    stop_rx();
}

bool LSLBridgeImpl::rx_running() const {
    return rx_running_.load();
}

// -------------------- TX --------------------
const char* LSLBridgeImpl::marker_to_text_(Marker m) {
    switch (m) {
    case Marker::SPACE_DOWN: return "SPACE_DOWN";
    case Marker::SPACE_UP:   return "SPACE_UP";
    default:                 return "UNKNOWN_MARKER";
    }
}

const char* LSLBridgeImpl::direction_to_text_(Direction d) {
    // ⚠️ 반드시 네 Direction enum에 맞춰야 함
    // 임시: 컴파일이라도 되게 default만
    (void)d;
    return "DIRECTION";
}

void LSLBridgeImpl::send_marker(Marker marker, double /*timestamp_sec*/) {
    std::string sample[1];
    sample[0] = marker_to_text_(marker);
    tx_marker_outlet_.push_sample(sample);
}

void LSLBridgeImpl::send_direction(Direction direction, double /*timestamp_sec*/) {
    std::string sample[1];
    sample[0] = direction_to_text_(direction);
    tx_direction_outlet_.push_sample(sample);
}

void LSLBridgeImpl::send_marker_text(const std::string& text, double /*timestamp_sec*/) {
    std::string sample[1];
    sample[0] = text;
    tx_marker_outlet_.push_sample(sample);
}

// -------------------- RX --------------------
void LSLBridgeImpl::start_rx(OnMarkerText cb) {
    if (rx_running_.load()) return;

    rx_cb_ = std::move(cb);
    rx_running_.store(true);
    rx_th_ = std::thread(&LSLBridgeImpl::rx_thread_main_, this);
}

void LSLBridgeImpl::stop_rx() {
    if (!rx_running_.load()) return;

    rx_running_.store(false);
    if (rx_th_.joinable()) rx_th_.join();
}

void LSLBridgeImpl::rx_thread_main_() {
    try {
        auto results = lsl::resolve_stream("name", rx_stream_name_, 1, 5.0);
        if (results.empty()) {
            rx_running_.store(false);
            return;
        }

        lsl::stream_inlet inlet(results[0]);
        std::vector<std::string> sample(1);

        while (rx_running_.load()) {
            double lsl_ts = inlet.pull_sample(sample, 0.2);
            if (!rx_running_.load()) break;
            if (lsl_ts == 0.0) continue;

            if (rx_cb_) rx_cb_(sample[0], lsl_ts);
        }
    }
    catch (...) {
        rx_running_.store(false);
    }
}
