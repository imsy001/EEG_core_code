#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <mutex>
#include <vector>

// Forward declarations to avoid pulling heavy headers into the .hpp
class EEGDevice;
class Controller;
class LSLBridge;

// Simple GUI app wrapper (GLFW + OpenGL3 + Dear ImGui)
class EEGGuiApp final {
public:
    EEGGuiApp();
    ~EEGGuiApp();

    // Runs the GUI loop until quit. Returns process exit code.
    int run();

private:
    // lifecycle
    bool init_window_and_imgui_();
    void shutdown_window_and_imgui_();

    // device control
    bool start_streaming_();
    void stop_streaming_();

    // per-frame
    void draw_ui_();

private:
    // ---- UI / Window ----
    struct Impl;
    std::unique_ptr<Impl> impl_; // holds GLFWwindow* and backend state

    // ---- EEG core wiring ----
    std::unique_ptr<EEGDevice> dev_;
    std::unique_ptr<Controller> controller_;
    std::unique_ptr<LSLBridge> lsl_;

    // ---- state flags ----
    std::atomic<bool> streaming_{false};
    std::atomic<bool> accepting_{false};
    bool want_quit_ = false;

    // ---- config / messages ----
    int lx_device_id_ = 300;
    int numsample_return_ = 32;
    int num_channels_ = 35;

    std::string last_status_;
    std::string last_error_;

private:
    // ---- GUI ring buffer ----
    mutable std::mutex vis_mu_;
    std::vector<float> vis_ring_;     // interleaved [sample][channel]
    int vis_capacity_ = 2000;         // samples stored for display
    int vis_nch_ = 0;                 // channels tracked
    int vis_write_ = 0;               // write index (0..vis_capacity_-1)
    bool vis_ready_ = false;

    // UI selections
    int ui_channel_ = 0;              // which channel to plot
    int ui_plot_n_ = 600;             // how many samples to plot
};
