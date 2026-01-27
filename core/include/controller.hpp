#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "types.hpp"
#include "config.hpp"
#include "ring_buffer.hpp"
#include "epoch_buffer.hpp"

#include "device/include/eeg_device.hpp"
#include "device/include/lsl_bridge.hpp"

// - command queue (GUI -> core)
// - device streaming start/stop
// - stats snapshot for GUI

// - marker-based epoching


class Controller {
public:
    enum class State : uint8_t {
        STOPPED,        // device closed
        DEVICE_OPEN,    // device open, not streaming
        STREAMING       // streaming active
    };

    // Sub-state (valid only when State::RUNNING)
    enum class RunningMode : uint8_t {
        INFERENCE_ONLY, // run classifier, output Direction
        LABELLING_ONLY,  // collect labeled epochs for training
        INFERENCE_AND_LABELLING // run classifier, ouput Direction, collect labeled epchs for training
    };

    struct Stats {
        double sample_rate_hz = 0.0;
        uint64_t samples_total = 0;
        uint64_t dropped_samples = 0;
        uint64_t marker_count = 0;
        bool device_ok = false;
        bool lsl_ok = false;
        std::string last_error;
    };

    // ---------- Commands (GUI -> Core thread) ----------
    struct CmdStart {}; //device open, start_streaming(callback), // marker listener 활성화, state = RUNNING
    struct CmdStop {}; //현재 epoch가 있으면 종료, streaming stop, device close, //writer flush, state = STOPPED

    struct CmdDeviceOpen {};
    struct CmdDeviceClose {};
    struct CmdStreamStart {};
    struct CmdStreamStop {};

    struct CmdSetRunningMode { RunningMode mode; };

    struct CmdSetOutputDir { std::string dir; };
    struct CmdArmRecording { bool armed; };   // marker epoching을 “켜거나 끄라” armed = true → SPACE_DOWN/UP 반응 armed = false → marker 무시
    struct CmdSaveNow {};                     // Force save current epoch buffer (debug)
    struct CmdClearEpoch {};                  // Clear current epoch buffer

    struct CmdHandleEpoch {
        std::vector<EEGSample> epoch; // moved from marker thread
        double ts = 0.0;              // epoch end timestamp (or marker ts)
    };

    using Command = std::variant<
        CmdDeviceOpen, CmdDeviceClose, CmdStreamStart, CmdStreamStop,
        CmdSetRunningMode, CmdSetOutputDir, CmdArmRecording, CmdSaveNow, CmdClearEpoch,
        CmdHandleEpoch
    >;

public:
    Controller(EEGDevice& device, LSLBridge& lsl);
    ~Controller();

    // GUI thread: non-blocking
    void post(Command cmd); // GUI calls post(CmdStart{}), post(CmdStop{}), post(CmdSetOutputDir{...}), etc.

    // GUI-safe reads
    State get_state() const noexcept { return state_.load(std::memory_order_relaxed); }
    RunningMode get_RunningMode() const;
    Stats get_stats() const;

    // Called by device streaming callback thread (real-time-ish)
    void on_eeg_sample(const EEGSample& sample);

    // Called by marker listener thread (LSL)
    // Provide timestamp_sec if you have it; otherwise pass host time.
    void on_marker(Marker marker, double timestamp_sec);

private:
    RingBuffer ring_{4096};   // display용
    EpochBuffer epoch_;       // SPACE_DOWN~UP

private:
    // Core thread loop
    void core_loop();
    void process_commands(); // takes commands posted by GUI (post(cmd)) and executes them.

    // Executed only on core thread
    void do_start();
    void do_stop();

    void do_device_open();
    void do_device_close();
    void do_stream_start();
    void do_stream_stop();

    void do_change_running_mode();
    void do_save_now();
    void do_clear_epoch();
    // controller.hpp (private에 추가)
    void do_handle_epoch(std::vector<EEGSample>&& epoch, double ts);


    // Epoch helpers (thread-safe)
    void start_epoch(double ts); //Purpose: begin an epoch (a “meaningful segment” of EEG) at time ts. 
                                 //Typical trigger: a marker like SPACE_DOWN.
    void end_epoch(double ts); //Purpose: end the epoch at time ts, then hand the collected samples off to saving/inference.
                               //Typical trigger: SPACE_UP marker, or a safety timeout.

    // fixed-length epoch (SPACE_DOWN + 2s)
    std::atomic<bool> fixed_epoch_armed_{ false };
    std::atomic<double> fixed_epoch_end_ts_{ 0.0 }; // seconds


    // Queue
    void enqueue(Command&& cmd); //Purpose: push a command into the command queue safely.
                                 //lock the command queue mutex, push the command

private:
    EEGDevice& device_;
    LSLBridge& lsl_;

    // Core thread + command queue
    std::atomic<bool> core_running_{true};
    std::thread core_thread_;

    mutable std::mutex cmd_mtx_;
    std::queue<Command> cmd_q_;

    // State
    std::atomic<State> state_{State::STOPPED};
    RunningMode mode_ = RunningMode::INFERENCE_ONLY;
    std::atomic<bool> do_label_{false};
    std::atomic<bool> do_infer_{true};

    // Config/state
    std::string output_dir_;
    std::atomic<bool> armed_{true}; // if false, marker epoching ignored

    // Stats (snapshot)
    mutable std::mutex stats_mtx_;
    Stats stats_;
};



/*1️⃣ Controller는 아무것도 소유하지 않는다

EEGDevice ❌ ownership 없음

LSLBridge ❌ ownership 없음

UI ❌

Thread ❌

2️⃣ 진입점이 딱 두 개뿐
on_eeg_sample(...)
on_marker(...)


이건 의도적이다.

EEG → continuous signal

Marker → discrete event

👉 뇌-실험 모델 그대로

Epoch buffer ✅ (저장/학습용)

네가 말한:

“Dedicated epoch buffer 유지 (저장용, loss 없음)”

이게 정확히 구현되는 지점이다.

4️⃣ dispatch는 “의미”만 전달
dispatch_direction(Direction::Left);

👉 Controller는 **의도(intent)**만 안다.*/
