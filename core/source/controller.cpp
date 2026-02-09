/*controller.cpp

It should handle:

SPACE_DOWN / SPACE_UP state machine

Epoch buffer management

Trial lifecycle


High-level commands like:

EpochBuffer.start()
EpochBuffer.end()

emit_command(Direction::Left)
*/

#include "core/include/controller.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <memory>
#include <string>
#include <sstream>
#include <unordered_map>
#include <vector>


namespace config {
    inline constexpr double kPreSec = 1.0;
    inline constexpr double kPostSec = 1.0;
}


namespace {

    static std::vector<std::string> split_by(const std::string& s, char delim) {
        std::vector<std::string> out;
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, delim)) out.push_back(item);
        return out;
    }

    static std::unordered_map<std::string, std::string> parse_space_kv(const std::string& text) {
        // Expect: "SPACE|phase=EXEC|trial=42|dir=MOVE_Left|unity_rt=...|lsl=..."
        std::unordered_map<std::string, std::string> kv;
        auto parts = split_by(text, '|');
        if (parts.empty()) return kv;
        if (parts[0] != "SPACE") return kv;

        for (size_t i = 1; i < parts.size(); ++i) {
            auto pos = parts[i].find('=');
            if (pos == std::string::npos) continue;
            kv.emplace(parts[i].substr(0, pos), parts[i].substr(pos + 1));
        }
        return kv;
    }

    static const char* marker_to_cstr(Marker m) {
        switch (m) {
        case Marker::SPACE_DOWN: return "SPACE_DOWN";
        case Marker::SPACE_UP:   return "SPACE_UP";
        default:                 return "UNKNOWN";
        }
    }

    static const char* direction_to_cstr(const std::optional<Direction>& d) {
        if (!d) return "NONE";
        switch (*d) {
        case Direction::Left:     return "Left";
        case Direction::Right:    return "Right";
        case Direction::Up:       return "Up";
        case Direction::Down:     return "Down";
        case Direction::ZOOM_IN:  return "ZOOM_IN";
        case Direction::ZOOM_OUT: return "ZOOM_OUT";
        default:                  return "UNKNOWN";
        }
    }

    inline const char* marker_to_str(Marker m) {
        switch (m) {
        case Marker::SPACE_DOWN: return "SPACE_DOWN";
        case Marker::SPACE_UP:   return "SPACE_UP";
        default:                 return "UNKNOWN";
        }
    }

    inline const char* direction_to_str(Direction d) {
        switch (d) {
        case Direction::Left:     return "Left";
        case Direction::Right:    return "Right";
        case Direction::Up:       return "Up";
        case Direction::Down:     return "Down";
        case Direction::ZOOM_IN:  return "ZOOM_IN";
        case Direction::ZOOM_OUT: return "ZOOM_OUT";
        default:                  return "UNKNOWN";
        }
    }


void save_epoch_binary(const std::string& output_dir,
                       const std::vector<EEGSample>& epoch,
                       double ts) {
    if (output_dir.empty() || epoch.empty()) return;

    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) return;

    const auto channel_count = static_cast<uint32_t>(epoch.front().channels.size());
    if (channel_count == 0) return;

    const auto sample_count = static_cast<uint32_t>(epoch.size());
    const auto filename =
        output_dir + "/epoch_" + std::to_string(ts) + ".bin";

    std::ofstream out(filename, std::ios::binary);
    if (!out) return;

    out.write(reinterpret_cast<const char*>(&sample_count), sizeof(sample_count));
    out.write(reinterpret_cast<const char*>(&channel_count), sizeof(channel_count));
    out.write(reinterpret_cast<const char*>(&ts), sizeof(ts));

    for (const auto& s : epoch) {
        out.write(reinterpret_cast<const char*>(&s.timestamp_sec),
                  sizeof(s.timestamp_sec));
        out.write(reinterpret_cast<const char*>(s.channels.data()),
                  sizeof(float) * channel_count);
    }
}

void save_epoch_csv(const std::string& output_dir,
    const std::vector<EEGSample>& epoch,
    const EpochMeta& meta)
{
    if (output_dir.empty() || epoch.empty()) return;

    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) return;

    const int chN = static_cast<int>(epoch.front().channels.size());
    if (chN <= 0) return;

    const std::string filename =
        output_dir + "/epoch_" + std::to_string(meta.trigger_ts) + ".csv";

    std::ofstream out(filename);
    if (!out) { std::cout << "[SAVE] open failed: " << filename << "\n"; return; }

    out << std::fixed << std::setprecision(6);

    // =========================
    // Metadata (CSV 앞부분)
    // =========================
    // CSV 파서가 싫어하면 '#' 대신 다른 prefix로 바꿔도 됨
    out << "# label: " << meta.label << "\n";
    // 원하면 남겨도 되는 메타데이터 (요청한 것들은 "컬럼에서만" 제거하는 거라면 여기에 둬도 됨)
    // out << "# trigger_ts: " << meta.trigger_ts << "\n";
    // out << "# marker: " << marker_to_str(meta.marker) << "\n";
    // out << "# trial_id: " << meta.trial_id << "\n";
    // out << "# direction: " << direction_to_cstr(meta.direction) << "\n";

    // =========================
    // Header (데이터 컬럼)
    // =========================
    out << "timestamp_sec";
    for (int ch = 0; ch < chN; ++ch) out << ",ch" << ch;
    out << "\n";

    // =========================
    // Data rows
    // =========================
    for (const auto& s : epoch) {
        out << s.timestamp_sec;
        for (int ch = 0; ch < chN; ++ch) out << "," << s.channels[ch];
        out << "\n";
    }
}



//inference 빠르게 하기 위한 단순화 버전
Direction infer_direction_fast(const std::vector<EEGSample>& epoch) {
    if (epoch.empty() || epoch.front().channels.empty()) return Direction::Left;
    double acc = 0.0;
    for (const auto& s : epoch) acc += s.channels[0];
    return acc >= 0.0 ? Direction::Right : Direction::Left;
}
} // namespace





void Controller::cut_and_post_pre_only_(double ts_exec, const EpochMeta& meta)
{
    log_push_("[CUT] enter");

    if (!armed_.load(std::memory_order_relaxed)) {
        log_push_("[CUT] return: armed=false");
        return;
    }
    if (state_.load(std::memory_order_relaxed) != State::STREAMING) {
        log_push_("[CUT] return: not streaming");
        return;
    }

    const double t0 = ts_exec - config::kPreSec;
    auto v = ring_.slice(t0, ts_exec);

    {
        std::ostringstream oss;
        oss << "[CUT] n=" << v.size()
            << " t0=" << std::fixed << std::setprecision(6) << t0
            << " ts=" << ts_exec;
        log_push_(oss.str());
    }

    if (v.empty()) {
        log_push_("[CUT] return: slice empty");
        return;
    }

    const double dur = v.back().timestamp_sec - v.front().timestamp_sec;
    if (dur < config::kMinEpochSeconds) {
        std::ostringstream oss;
        oss << "[CUT] return: dur too short dur=" << dur;
        log_push_(oss.str());
        return;
    }

    auto payload = std::make_shared<std::vector<EEGSample>>(std::move(v));
    std::shared_ptr<const std::vector<EEGSample>> ro = payload;

    if (do_label_.load(std::memory_order_relaxed)) {
        log_push_("[CUT] posting CmdSaveEpoch");
        post(CmdSaveEpoch{ ro, meta });
    }
    else {
        log_push_("[CUT] do_label=false (skip save)");
    }

    if (do_infer_.load(std::memory_order_relaxed)) {
        log_push_("[CUT] posting CmdInferEpoch");
        post(CmdInferEpoch{ ro, meta });
    }
}



void Controller::set_pending_meta_(const EpochMeta& m) {
    {
        std::lock_guard<std::mutex> lk(pending_meta_mtx_);
        pending_meta_ = m;
    }
    pending_meta_valid_.store(true, std::memory_order_release);
}

EpochMeta Controller::consume_pending_meta_(double fallback_trigger_ts, Marker fallback_marker) {
    EpochMeta m;
    bool ok = pending_meta_valid_.exchange(false, std::memory_order_acq_rel);
    if (ok) {
        std::lock_guard<std::mutex> lk(pending_meta_mtx_);
        m = pending_meta_;
    }
    else {
        m.trigger_ts = fallback_trigger_ts;
        m.marker = fallback_marker;
        m.direction.reset();
    }
    
    return m;
}

// need change thread safety
Controller::Controller(EEGDevice& device, LSLBridge& lsl)
    : device_(device), lsl_(lsl) {
    core_thread_ = std::thread([this] { core_loop(); });
}

Controller::~Controller() {
    core_running_.store(false, std::memory_order_release);
    cmd_cv_.notify_all();                 // wake core_loop if it’s waiting
    if (core_thread_.joinable()) core_thread_.join();
}

void Controller::post(Command cmd) {
    enqueue(std::move(cmd));
}

void Controller::enqueue(Command&& cmd) {
    {
        std::lock_guard<std::mutex> lk(cmd_mtx_);
        cmd_q_.push(std::move(cmd));
    }
    cmd_cv_.notify_one();
}

Controller::Stats Controller::get_stats() const {
    std::lock_guard<std::mutex> lk(stats_mtx_);
    return stats_;
}

Controller::RunningMode Controller::get_running_mode() const {
	return mode_.load(std::memory_order_relaxed);
}

void Controller::core_loop() {
    while (core_running_.load(std::memory_order_acquire)) {
        process_commands();

        std::unique_lock<std::mutex> lk(cmd_mtx_);
        cmd_cv_.wait(lk, [&] {
            return !cmd_q_.empty() ||
                !core_running_.load(std::memory_order_acquire);
            });
        // loop continues; process_commands() will swap and handle cmds
    }

    // shutdown path (your existing cleanup)
    do_stream_stop();
    do_device_close();

    epoch_.clear();
    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.device_ok = false;
        stats_.lsl_ok = false;
    }
    state_.store(State::BASE, std::memory_order_relaxed);
}




void Controller::process_commands() {
    std::queue<Command> local;
    {
        std::lock_guard<std::mutex> lk(cmd_mtx_);
        std::swap(local, cmd_q_);
    }

    while (!local.empty()) {
        auto cmd = std::move(local.front());
        local.pop();

        std::visit([&](auto&& c) {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, CmdStart>) {
                do_start();
            } else if constexpr (std::is_same_v<T, CmdStop>) {
                do_stop();
            } 
            
              else if constexpr (std::is_same_v<T, CmdSetOutputDir>) {
                output_dir_ = std::move(c.dir);
            } else if constexpr (std::is_same_v<T, CmdSetRunningMode>) {
                mode_.store(c.mode, std::memory_order_relaxed);
                do_change_running_mode();
            } else if constexpr (std::is_same_v<T, CmdArmRecording>) {
                armed_.store(c.armed, std::memory_order_relaxed);
            } else if constexpr (std::is_same_v<T, CmdClearEpoch>) {
                do_clear_epoch();
            } 

              else if constexpr (std::is_same_v<T, CmdSaveEpoch>) {
                do_save_epoch(c.epoch, c.meta);
            } else if constexpr (std::is_same_v<T, CmdInferEpoch>) {
                do_infer_epoch(c.epoch, c.meta);
            }
            
              else if constexpr (std::is_same_v<T, CmdDeviceOpen>) {
                do_device_open();
            } else if constexpr (std::is_same_v<T, CmdDeviceClose>) {
                do_device_close();
            } else if constexpr (std::is_same_v<T, CmdStreamStart>) {
                do_stream_start();
            } else if constexpr (std::is_same_v<T, CmdStreamStop>) {
                do_stream_stop();
            }
        }, std::move(cmd));
    }
}

// Controller.cpp
std::string Controller::last_status() const {
    std::lock_guard<std::mutex> lk(stats_mtx_);
    return stats_.last_status;
}
std::string Controller::last_error() const {
    std::lock_guard<std::mutex> lk(stats_mtx_);
    return stats_.last_error;
}

Controller::Stats Controller::stats() const {
    std::lock_guard<std::mutex> lk(stats_mtx_);
    return stats_;   // copy
}
bool Controller::is_streaming() const {
    return state_.load(std::memory_order_relaxed) == State::STREAMING;
}

// ------------------------Start/Stop---------------------------
void Controller::do_stop() {
    accepting_.store(false, std::memory_order_release); // stop callback pipeline ASAP
    core_running_.store(false, std::memory_order_release);
    cmd_cv_.notify_all();
}

void Controller::do_start() {
    // Define CmdStart behavior: "go to streaming"
    do_device_open();   // safe if already open; your state machine handles it
    do_stream_start();  // safe if already streaming; your state machine handles it
}



// ------------------------Device_open/close---------------------------
void Controller::do_device_open() {
	auto st = state_.load(std::memory_order_relaxed);

    if (st == State::DEVICE_OPEN) {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.device_ok = true;
		stats_.last_error.clear();
		stats_.last_status = "Device already open";
        return;
    }

    if (st == State::STREAMING) {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.device_ok = true;
        stats_.last_error.clear();
        stats_.last_status = "Cannot open device while streaming";
        return;
    }

    if (!device_.open()) {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.device_ok = false;
        stats_.last_error = device_.last_error();
        stats_.last_status = "Device open failed";
        return;
    }

    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.device_ok = true;
        stats_.last_error.clear();
        stats_.last_status = "Device opened";
    }

    state_.store(State::DEVICE_OPEN, std::memory_order_relaxed);
}

void Controller::do_device_close() {
    auto st = state_.load(std::memory_order_relaxed);
    if (st == State::BASE) {
        std::lock_guard<std::mutex> lk(stats_mtx_);
		stats_.device_ok = false;
		stats_.last_error.clear();
        stats_.last_status = "Device already closed";
        return;
	}

    if (st == State::STREAMING) {
        if (device_.is_streaming()) device_.stop_streaming();
    }

    epoch_.clear();
    if (device_.is_open()) device_.close();

    state_.store(State::BASE, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.device_ok = false;
        stats_.lsl_ok = false;
        stats_.last_error.clear();
        stats_.last_status = "Device closed";
    }
}


// ------------------------Stream_start/stop---------------------------
void Controller::do_stream_start() {
    auto st = state_.load(std::memory_order_relaxed);

    if (st == State::STREAMING) {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.last_error.clear();
        stats_.last_status = "Already streaming";
        return;
    }

    // Ensure device is open
    if (st == State::BASE) {
        if (!device_.open()) {
            std::lock_guard<std::mutex> lk(stats_mtx_);
            stats_.device_ok = false;
            stats_.last_error = device_.last_error();
            stats_.last_status = "Device open failed";
            return;
        }

        state_.store(State::DEVICE_OPEN, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(stats_mtx_);
            stats_.device_ok = true;
            stats_.last_error.clear();
            stats_.last_status = "Device opened";
        }
    }

    // st is now DEVICE_OPEN (or was already)
    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.last_error.clear();
        stats_.last_status = "Starting streaming...";
    }

    {
        std::lock_guard<std::mutex> lk(vis_mtx_);
        if (vis_capacity_ <= 0) vis_capacity_ = 5000;
        vis_nch_ = 0;
        vis_ring_.clear();
        vis_write_ = 0;
        vis_ready_ = true;
    }

    const bool ok = device_.start_streaming([this](const EEGSample& s) {
        if (!accepting_.load(std::memory_order_acquire)) return;

        // 1) core pipeline
        this->on_eeg_sample(s);

        // 2) controller-owned visualization ring
        this->push_vis_sample_(s);
        });

    if (!ok) {
        accepting_.store(false, std::memory_order_release);

        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.last_error = device_.last_error();
        stats_.last_status = "start_streaming() failed";
        return;
    }

    accepting_.store(true, std::memory_order_release);

    state_.store(State::STREAMING, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.last_error.clear();
        stats_.last_status = "Streaming started";
        // stats_.lsl_ok = true; // only if you actually start LSL here
    }
}


void Controller::do_stream_stop() {
    auto st = state_.load(std::memory_order_relaxed);

    if (st != State::STREAMING) {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.last_error.clear();
        stats_.last_status = "Not streaming";
        return;
    }

    // Stop accepting samples first (important: callback thread gate)
    accepting_.store(false, std::memory_order_release);

    if (device_.is_streaming()) {
        device_.stop_streaming(); // if this can fail, capture device_.last_error()
    }

    state_.store(State::DEVICE_OPEN, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.last_error.clear();
        stats_.last_status = "Streaming stopped";
    }
}


//--------------------------Change_mode-----------------------
void Controller::do_change_running_mode() {
    epoch_.clear();

    const auto m = mode_.load(std::memory_order_relaxed);

    const bool do_label =
        (m == RunningMode::LABELLING_ONLY) ||
        (m == RunningMode::INFERENCE_AND_LABELLING);

    const bool do_infer =
        (m == RunningMode::INFERENCE_ONLY) ||
        (m == RunningMode::INFERENCE_AND_LABELLING);

    do_label_.store(do_label, std::memory_order_relaxed);
    do_infer_.store(do_infer, std::memory_order_relaxed);

    // choose your policy:
    armed_.store(do_label || do_infer, std::memory_order_relaxed);


    // ✅ cancel any pending fixed-epoch from previous mode
    fixed_epoch_armed_.store(false, std::memory_order_release);
}



// -------------------- Sample callback thread --------------------

void Controller::on_eeg_sample(const EEGSample& sample) {
    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.samples_total++;
    }

    ring_.push(sample);

    if (!fixed_epoch_armed_.load(std::memory_order_acquire))
        return;

    const double end_ts = fixed_epoch_end_ts_.load(std::memory_order_acquire);

    // post 구간 수집 (end_ts까지)
    if (sample.timestamp_sec <= end_ts) {
        std::lock_guard<std::mutex> lk(epoch_mtx_);
        epoch_.push(sample);
    }

    // end 도달 → finalize 1회
    if (sample.timestamp_sec >= end_ts) {
        bool expected = true;
        if (!fixed_epoch_armed_.compare_exchange_strong(
            expected, false, std::memory_order_acq_rel))
            return;

        std::vector<EEGSample> v;
        {
            std::lock_guard<std::mutex> lk(epoch_mtx_);
            v = epoch_.end(end_ts, config::kMinEpochSeconds);
        }
        if (v.empty()) return;

        auto payload = std::make_shared<std::vector<EEGSample>>(std::move(v));
        std::shared_ptr<const std::vector<EEGSample>> ro = payload;

        // ✅ meta는 SPACE_DOWN 때 저장해둔 것을 가져온다
        EpochMeta meta = consume_pending_meta_(/*fallback*/ end_ts, Marker::SPACE_DOWN);
        // 여기서 meta.trigger_ts가 SPACE_DOWN로 유지되게 하고 싶으면:
        // consume_pending_meta_가 "덮어쓰기 안 하게" 구현되어 있어야 함.

        if (do_label_.load(std::memory_order_relaxed))
            post(CmdSaveEpoch{ ro, meta });

        if (do_infer_.load(std::memory_order_relaxed))
            post(CmdInferEpoch{ ro, meta });
    }
}





//-------------------- Visualization ring buffer --------------------
void Controller::push_vis_sample_(const EEGSample& s) {
    const int nch = static_cast<int>(s.channels.size());

    if (vis_capacity_ <= 0) return; // should never happen if init is correct
    if (nch <= 0) return;

    std::lock_guard<std::mutex> lk(vis_mtx_);
    if (!vis_ready_) return;

    if (nch != vis_nch_) {
        vis_nch_ = nch;
        vis_ring_.assign(vis_capacity_ * vis_nch_, 0.0f);
        vis_write_ = 0;
    }

    float* dst = &vis_ring_[vis_write_ * vis_nch_];
    for (int c = 0; c < vis_nch_; ++c)
        dst[c] = s.channels[c];

    vis_write_ = (vis_write_ + 1) % vis_capacity_;
}



// -------------------- Marker thread --------------------

// -------------------- ts ~ ts + 2 epoch save ----------------
void Controller::on_marker(Marker marker, double ts) {
    EpochMeta meta;
    meta.trigger_ts = ts;
    meta.marker = marker;
    set_pending_meta_(meta);

    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.marker_count++;
    }

    if (!config::kUseMarkerEpoching) return;
    if (!armed_.load(std::memory_order_relaxed)) return;
    if (state_.load(std::memory_order_relaxed) != State::STREAMING) return;

    if (marker == Marker::SPACE_DOWN) start_epoch(ts);
    else if (marker == Marker::SPACE_UP) end_epoch(ts);

}

// -------------------- ts - 2 ~ ts epoch save ----------------
void Controller::on_marker_pre(Marker marker, double ts) {
    EpochMeta meta;
    meta.trigger_ts = ts;
    meta.marker = marker;
    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.marker_count++;
    }

    if (!config::kUseMarkerEpoching) return;
    if (!armed_.load(std::memory_order_relaxed)) return;
    if (state_.load(std::memory_order_relaxed) != State::STREAMING) return;

    if (marker != Marker::SPACE_DOWN) return;

    const double t0 = ts - 2.0;
    auto v = ring_.slice(t0, ts);

    // Enforce min duration (optional but recommended)
    if (v.empty()) return;
    const double dur = v.back().timestamp_sec - v.front().timestamp_sec;
    if (dur < config::kMinEpochSeconds) return;

    auto payload = std::make_shared<std::vector<EEGSample>>(std::move(v));
    std::shared_ptr<const std::vector<EEGSample>> ro = payload;

    if (do_label_.load(std::memory_order_relaxed))
        post(CmdSaveEpoch{ ro, meta });

    if (do_infer_.load(std::memory_order_relaxed))
        post(CmdInferEpoch{ ro, meta });
}

// -------------------- ts - 2 ~ ts + 2 epoch save ----------------
void Controller::on_marker_pre_post(Marker marker, double ts_down) {
    if (!config::kUseMarkerEpoching) return;
    if (!armed_.load(std::memory_order_relaxed)) return;
    if (state_.load(std::memory_order_relaxed) != State::STREAMING) return;
    if (marker != Marker::SPACE_DOWN) return;

    // 이미 진행중이면 정책 선택: 무시 or 리셋
    if (fixed_epoch_armed_.load(std::memory_order_acquire)) {
        // 여기서는 "무시" 권장 (중복 trial 방지)
        return;
    }

    // 1) meta 저장 (SPACE_DOWN 기준)
    EpochMeta meta;
    meta.trigger_ts = ts_down;
    meta.marker = Marker::SPACE_DOWN;
    meta.direction.reset(); // label 파싱하면 여기 넣기
    set_pending_meta_(meta);

    // 2) pre slice seed
    const double t0 = ts_down - config::kPreSec;
    const double tend = ts_down + config::kPostSec;

    auto pre = ring_.slice(t0, ts_down);
    if (pre.empty()) return;

    {
        std::lock_guard<std::mutex> lk(epoch_mtx_); // ✅ thread safety
        epoch_.clear();
        epoch_.seed(t0, std::move(pre));
    }

    // 3) arm post collection
    fixed_epoch_end_ts_.store(tend, std::memory_order_release);
    fixed_epoch_armed_.store(true, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.last_status = "Epoch armed (pre+post)";
    }
}




// -------------------- Epoch helpers --------------------

void Controller::start_epoch(double ts) {
    epoch_.start(ts);

    // Optional: reflect marker out
    // lsl_.send_marker(Marker::SPACE_DOWN, ts);
    fixed_epoch_end_ts_.store(ts + 2.0, std::memory_order_release);
    fixed_epoch_armed_.store(true, std::memory_order_release);
}

void Controller::end_epoch(double ts) {
    fixed_epoch_armed_.store(false, std::memory_order_release); // optional but recommended

    auto v = epoch_.end(ts, config::kMinEpochSeconds);
    if (v.empty()) return;

    auto payload = std::make_shared<std::vector<EEGSample>>(std::move(v));
    std::shared_ptr<const std::vector<EEGSample>> ro = payload; // implicit upcast to const

    EpochMeta meta;
    meta.trigger_ts = ts;
    meta.marker = Marker::SPACE_UP;   // end marker 기준으로 저장 (원하면 SPACE_DOWN으로도 가능)
    meta.direction.reset();

    if (do_label_.load(std::memory_order_relaxed))
        post(CmdSaveEpoch{ ro, meta });

    if (do_infer_.load(std::memory_order_relaxed))
        post(CmdInferEpoch{ ro, meta });

}

void Controller::do_save_epoch(std::shared_ptr<const std::vector<EEGSample>> epoch,
    const EpochMeta& meta)
{
    {
        std::ostringstream oss;
        oss << "[SAVE] enter outdir=" << output_dir_
            << " epoch_null=" << (!epoch)
            << " epoch_size=" << (epoch ? epoch->size() : 0)
            << " do_label=" << do_label_.load();
        log_push_(oss.str());
    }

    if (!epoch || epoch->empty()) {
        log_push_("[SAVE] return: epoch empty/null");
        return;
    }
    if (!do_label_.load(std::memory_order_relaxed)) {
        log_push_("[SAVE] return: do_label=false");
        return;
    }
    if (output_dir_.empty()) {
        log_push_("[SAVE] WARNING: output_dir empty");
        // 그래도 save_epoch_csv가 return하긴 함. 원하면 여기서 return해도 됨.
    }

    save_epoch_csv(output_dir_, *epoch, meta);
    log_push_("[SAVE] save_epoch_csv done");
}



void Controller::do_infer_epoch(std::shared_ptr<const std::vector<EEGSample>> epoch,
    const EpochMeta& meta)
{
    if (!epoch || epoch->empty()) return;
    if (!do_infer_.load(std::memory_order_relaxed)) return;

    if (config::kEnableOnlineInference) {
        const auto dir = infer_direction_fast(*epoch);
        lsl_.send_direction(dir, meta.trigger_ts);
    }
}

    


// -------------------- Save/Epoch (core thread only) --------------------



// -------------------- Clear/Epoch (core thread only) --------------------
void Controller::do_clear_epoch() {
    std::cout << "[Controller] do_clear_epoch() called\n";
    epoch_.clear();
}

// ------------------- LSL_Marker ----------------------------------------
void Controller::on_marker_text(const std::string& text, double ts)
{
    // 1) New format first
    auto kv = parse_space_kv(text);
    if (!kv.empty()) {
        const std::string phase = (kv.count("phase") ? kv["phase"] : "");

        int trial_id = -1;
        if (kv.count("trial")) {
            try { trial_id = std::stoi(kv["trial"]); }
            catch (...) {}
        }

        std::string dir = (kv.count("dir") ? kv["dir"] : "");

        if (phase == "DECIDE") {
            std::lock_guard<std::mutex> lk(decide_mtx_);
            last_trial_id_ = trial_id;
            last_dir_label_ = dir;
            last_decide_ts_ = ts;

            {
                std::lock_guard<std::mutex> lk2(stats_mtx_);
                stats_.marker_count++;
                stats_.last_status = "Marker: DECIDE dir=" + dir;
            }
            return;
        }

        

        if (phase == "EXEC") {
            std::string label;
            int use_trial = trial_id;

            {
                std::lock_guard<std::mutex> lk(decide_mtx_);
                label = !dir.empty() ? dir : last_dir_label_;
                if (use_trial < 0) use_trial = last_trial_id_;
            }

            std::ostringstream oss;
            oss << "[MARKER] EXEC ts=" << std::fixed << std::setprecision(6) << ts
                << " outdir=" << output_dir_
                << " do_label=" << do_label_.load()
                << " armed=" << armed_.load()
                << " state=" << int(state_.load())
                << " trial=" << use_trial
                << " label=" << label;

            log_push_(oss.str());

            EpochMeta meta;
            meta.trigger_ts = ts;
            meta.marker = Marker::SPACE_DOWN;   // 또는 Marker::EXEC 새로 만들어도 됨
            meta.trial_id = use_trial;
            meta.label = label;
            meta.direction.reset();             // 아래 3번에서 매핑 넣으면 좋음

            {
                std::lock_guard<std::mutex> lk2(stats_mtx_);
                stats_.marker_count++;
                stats_.last_status = "Marker: EXEC label=" + label;
            }

            cut_and_post_pre_only_(ts, meta);
            return;
        }


        if (phase == "RESET") {
            std::lock_guard<std::mutex> lk(decide_mtx_);
            last_trial_id_ = -1;
            last_dir_label_.clear();
            last_decide_ts_ = 0.0;

            {
                std::lock_guard<std::mutex> lk2(stats_mtx_);
                stats_.marker_count++;
                stats_.last_status = "Marker: RESET";
            }
            return;
        }

        {
            std::lock_guard<std::mutex> lk2(stats_mtx_);
            stats_.last_status = "Unknown SPACE phase: " + phase;
        }
        return;
    }

    // 2) Backward compatibility: old markers
    if (text.rfind("SPACE_DOWN", 0) == 0) {
        on_marker_pre_post(Marker::SPACE_DOWN, ts);
        return;
    }
    if (text.rfind("SPACE_UP", 0) == 0) {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.marker_count++;
        stats_.last_status = "Marker: SPACE_UP";
        return;
    }

    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.last_status = "Unknown marker: " + text;
    }
}

void Controller::set_log_fn(LogFn fn) {
    log_fn_ = std::move(fn);
}

void Controller::log_push_(const std::string& s) {
    if (log_fn_) log_fn_(s);
}








