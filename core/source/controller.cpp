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

namespace {
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

Direction infer_direction_fast(const std::vector<EEGSample>& epoch) {
    if (epoch.empty() || epoch.front().channels.empty()) return Direction::Left;

    double acc = 0.0;
    for (const auto& s : epoch) {
        acc += s.channels[0];
    }
    return acc >= 0.0 ? Direction::Right : Direction::Left;
}
} // namespace

Controller::Controller(EEGDevice& device, LSLBridge& lsl)
    : device_(device), lsl_(lsl) {
    core_thread_ = std::thread([this] { core_loop(); });
}

Controller::~Controller() {
    // Request stop, then end core loop
    post(CmdStop{});
    core_running_.store(false, std::memory_order_relaxed);
    if (core_thread_.joinable()) core_thread_.join();
}

void Controller::post(Command cmd) {
    enqueue(std::move(cmd));
}

void Controller::enqueue(Command&& cmd) {
    std::lock_guard<std::mutex> lk(cmd_mtx_);
    cmd_q_.push(std::move(cmd));
}

Controller::Stats Controller::get_stats() const {
    std::lock_guard<std::mutex> lk(stats_mtx_);
    return stats_;
}

Controller::RunningMode Controller::get_RunningMode() const {
    return mode_;
}

void Controller::core_loop() {
    while (core_running_.load(std::memory_order_relaxed)) {
        process_commands();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // ensure shutdown even if app exits abruptly
    do_stop();
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
            } else if constexpr (std::is_same_v<T, CmdSetOutputDir>) {
                output_dir_ = std::move(c.dir);
            } else if constexpr (std::is_same_v<T, CmdSetRunningMode>) {
                mode_ = c.mode;
                do_change_running_mode();
            } else if constexpr (std::is_same_v<T, CmdArmRecording>) {
                armed_.store(c.armed, std::memory_order_relaxed);
            } else if constexpr (std::is_same_v<T, CmdSaveNow>) {
                do_save_now();
            } else if constexpr (std::is_same_v<T, CmdClearEpoch>) {
                do_clear_epoch();
            }
        }, std::move(cmd));
    }
}

// -------------------- Start/Stop (core thread only) --------------------

void Controller::do_start() {
    if (state_.load(std::memory_order_relaxed) != State::STOPPED) return;

    // 1) open device
    if (!device_.open()) {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.device_ok = false;
        stats_.last_error = device_.last_error();
        return;
    }

    // 2) start streaming (non-blocking per your interface contract)
    const bool ok = device_.start_streaming([this](const EEGSample& s) {
        this->on_eeg_sample(s);
    });

    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.device_ok = ok && device_.is_streaming();
        stats_.last_error = ok ? "" : device_.last_error();

        // If you have sample rate in info():
        // auto inf = device_.info();
        // stats_.sample_rate_hz = inf.sample_rate_hz;
    }

    if (!ok) {
        {
            std::lock_guard<std::mutex> lk(stats_mtx_);
            stats_.device_ok = false;
            stats_.last_error = device_.last_error();
        }
        device_.close();
        return;
    }

    // Optionally connect marker stream here if LSLBridge supports it
    // lsl_.connect();

    state_.store(State::RUNNING, std::memory_order_relaxed);
}

void Controller::do_stop() {
    if (state_.load(std::memory_order_relaxed) == State::STOPPED) return;

    epoch_.clear();

    // Stop streaming
    if (device_.is_streaming()) {
        device_.stop_streaming();
    }
    if (device_.is_open()) {
        device_.close();
    }

    // Optionally disconnect LSL
    // lsl_.disconnect();

    state_.store(State::STOPPED, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.device_ok = false;
        stats_.lsl_ok = false;
    }
}

void Controller::do_change_running_mode() {
    epoch_.clear();

    const bool do_label =
        mode_ == RunningMode::LABELLING_ONLY ||
        mode_ == RunningMode::INFERENCE_AND_LABELLING;
    const bool do_infer =
        mode_ == RunningMode::INFERENCE_ONLY ||
        mode_ == RunningMode::INFERENCE_AND_LABELLING;

    do_label_.store(do_label, std::memory_order_relaxed);
    do_infer_.store(do_infer, std::memory_order_relaxed);
}


// -------------------- Sample callback thread --------------------

void Controller::on_eeg_sample(const EEGSample& sample) {
    // Keep callback short. Minimal lock time.
    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.samples_total++;
    }

    ring_.push(sample);
    epoch_.push(sample);
}

// -------------------- Marker thread --------------------

void Controller::on_marker(Marker marker, double ts) {
    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.marker_count++;
    }

    if (!config::kUseMarkerEpoching) return;
    if (!armed_.load(std::memory_order_relaxed)) return;
    if (state_.load(std::memory_order_relaxed) != State::RUNNING) return;

    if (marker == Marker::SPACE_DOWN) {
        start_epoch(ts);
    } else if (marker == Marker::SPACE_UP) {
        end_epoch(ts);
    }
}

// -------------------- Epoch helpers --------------------

void Controller::start_epoch(double ts) {
    epoch_.start(ts);

    // Optional: reflect marker out
    // lsl_.send_marker(Marker::SPACE_DOWN, ts);
}

void Controller::end_epoch(double ts) {
    // EpochBuffer가 lock + active check + min duration check + move-out을 다 해줌
    auto to_save = epoch_.end(ts, config::kMinEpochSeconds);
    if (to_save.empty()) return;

    // 여기부터는 "무거운 일" (저장/추론/라벨링)
    // 이 함수는 marker thread에서 호출되므로, 여기서 파일 I/O를 하면 marker thread가 막힘.
    // 가능하면 core thread로 넘기는 구조가 더 좋음(아래 참고).

    std::cout << "[Controller] epoch ended, samples=" << to_save.size() << "\n";

    const bool do_label = do_label_.load(std::memory_order_relaxed);
    const bool do_infer = do_infer_.load(std::memory_order_relaxed);

    if (do_label) {
        save_epoch_binary(output_dir_, to_save, ts);
    }

    if (do_infer && config::kEnableOnlineInference) {
        const auto dir = infer_direction_fast(to_save);
        lsl_.send_direction(dir, ts);
    }

    // Optional: reflect marker out (보통은 Unity가 source이므로 필요 없음)
    // lsl_.send_marker(Marker::SPACE_UP, ts);
}

// -------------------- Save/Epoch (core thread only) --------------------
void Controller::do_save_now() {
    std::cout << "[Controller] do_save_now() called\n";
    // Force save current epoch buffer (for debugging)
    // Note: this may interrupt an ongoing epoch
    auto now = std::chrono::steady_clock::now();
    double ts = std::chrono::duration<double>(now.time_since_epoch()).count();
    end_epoch(ts);
}

// -------------------- Clear/Epoch (core thread only) --------------------
void Controller::do_clear_epoch() {
    std::cout << "[Controller] do_clear_epoch() called\n";
    epoch_.clear();
}
