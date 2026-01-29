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

void save_epoch_csv(const std::string& output_dir,
    const std::vector<EEGSample>& epoch,
    double ts) {
    if (output_dir.empty() || epoch.empty()) return;

    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) return;

    const int chN = (int)epoch.front().channels.size();
    if (chN <= 0) return;

    const std::string filename = output_dir + "/epoch_" + std::to_string(ts) + ".csv";

    std::ofstream out(filename);
    if (!out) return;

    // header
    out << "timestamp_sec";
    for (int ch = 0; ch < chN; ++ch) out << ",ch" << ch;
    out << "\n";

    out << std::fixed << std::setprecision(6);

    // rows
    for (const auto& s : epoch) {
		out << s.timestamp_sec; // timestamp
        for (int ch = 0; ch < chN; ++ch) {
            out << "," << s.channels[ch];
        }
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
                do_save_epoch(c.epoch, c.ts);
            } else if constexpr (std::is_same_v<T, CmdInferEpoch>) {
                do_infer_epoch(c.epoch, c.ts);
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
    epoch_.push(sample);


    // fixed epoch logic ...
    if (fixed_epoch_armed_.load(std::memory_order_acquire)) {
        const double end_ts = fixed_epoch_end_ts_.load(std::memory_order_acquire);
        if (sample.timestamp_sec >= end_ts) {
            bool expected = true;
            if (fixed_epoch_armed_.compare_exchange_strong(
                expected, false, std::memory_order_acq_rel)) {

                auto v = epoch_.end(end_ts, 0.0);
                if (v.empty()) return;

                auto payload = std::make_shared<std::vector<EEGSample>>(std::move(v));
                std::shared_ptr<const std::vector<EEGSample>> ro = payload;

                if (do_label_.load(std::memory_order_relaxed))
                    post(CmdSaveEpoch{ ro, end_ts });
                if (do_infer_.load(std::memory_order_relaxed))
                    post(CmdInferEpoch{ ro, end_ts });
            }
        }
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

void Controller::on_marker(Marker marker, double ts) {
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

    if (do_label_.load(std::memory_order_relaxed))
        post(CmdSaveEpoch{ ro, ts });

    if (do_infer_.load(std::memory_order_relaxed))
        post(CmdInferEpoch{ ro, ts });
}

void Controller::do_save_epoch(std::shared_ptr<const std::vector<EEGSample>> epoch, double ts) {
    if (!epoch || epoch->empty()) return;
    if (!do_label_.load(std::memory_order_relaxed)) return;

    save_epoch_binary(output_dir_, *epoch, ts);
    save_epoch_csv(output_dir_, *epoch, ts);
}

void Controller::do_infer_epoch(std::shared_ptr<const std::vector<EEGSample>> epoch, double ts) {
    if (!epoch || epoch->empty()) return;
    if (!do_infer_.load(std::memory_order_relaxed)) return;

    if (config::kEnableOnlineInference) {
        const auto dir = infer_direction_fast(*epoch);
        lsl_.send_direction(dir, ts);
    }
}
    


// -------------------- Save/Epoch (core thread only) --------------------



// -------------------- Clear/Epoch (core thread only) --------------------
void Controller::do_clear_epoch() {
    std::cout << "[Controller] do_clear_epoch() called\n";
    epoch_.clear();
}
