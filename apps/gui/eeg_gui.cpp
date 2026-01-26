#include "eeg_gui.hpp"

#include <iostream>
#include <utility>
#include <algorithm>

// Your project headers
#include "core/include/controller.hpp"
#include "device/include/lx_device_factory.hpp"

// ======================
// Dear ImGui + backends
// ======================
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

// ----------------------
// Dummy LSLBridge
// ----------------------
class DummyLSLBridge final : public LSLBridge {
public:
    void send_marker(Marker, double) override {}
    void send_direction(Direction, double) override {}
};

// ----------------------
// Private implementation
// ----------------------
struct EEGGuiApp::Impl {
    GLFWwindow* window = nullptr;
    const char* glsl_version = "#version 150"; // good default on macOS (OpenGL 3.2 core)
};

// ----------------------
// Ctor / Dtor
// ----------------------
EEGGuiApp::EEGGuiApp()
    : impl_(std::make_unique<Impl>()) {}

EEGGuiApp::~EEGGuiApp() {
    // Ensure we stop streaming before shutting down window
    stop_streaming_();
    shutdown_window_and_imgui_();
}

// ----------------------
// Init / Shutdown
// ----------------------
bool EEGGuiApp::init_window_and_imgui_() {
    if (!glfwInit()) {
        last_error_ = "glfwInit() failed";
        return false;
    }

    // macOS: OpenGL 3.2 Core
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    impl_->window = glfwCreateWindow(1100, 700, "eeg_gui", nullptr, nullptr);
    if (!impl_->window) {
        last_error_ = "glfwCreateWindow() failed";
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(impl_->window);
    glfwSwapInterval(1); // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplGlfw_InitForOpenGL(impl_->window, true)) {
        last_error_ = "ImGui_ImplGlfw_InitForOpenGL() failed";
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init(impl_->glsl_version)) {
        last_error_ = "ImGui_ImplOpenGL3_Init() failed";
        return false;
    }

    last_status_ = "GUI initialized";
    return true;
}

void EEGGuiApp::shutdown_window_and_imgui_() {
    if (impl_ && impl_->window) {
        // ImGui backends
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        // Window
        glfwDestroyWindow(impl_->window);
        impl_->window = nullptr;

        glfwTerminate();
    }
}

// ----------------------
// Device start/stop
// ----------------------
bool EEGGuiApp::start_streaming_() {
    if (streaming_.load(std::memory_order_acquire)) {
        last_status_ = "Already streaming";
        return true;
    }

    // Build device + controller once (or rebuild each time if you want)
    if (!dev_) {
        LXConfig cfg;
        cfg.lx_device_id = lx_device_id_;
        cfg.numsample_return = numsample_return_;
        cfg.num_channels = num_channels_;

        dev_ = make_lx_device(cfg);
        lsl_ = std::make_unique<DummyLSLBridge>();
        controller_ = std::make_unique<Controller>(*dev_, *lsl_);
    }

    {
        std::lock_guard<std::mutex> lk(vis_mu_);
        vis_nch_ = num_channels_;
        vis_ring_.assign(vis_capacity_ * vis_nch_, 0.0f);
        vis_write_ = 0;
        vis_ready_ = true;
        ui_channel_ = 0;
    }


    if (!dev_->open()) {
        last_error_ = dev_->last_error();
        last_status_ = "open() failed";
        return false;
    }

    accepting_.store(true, std::memory_order_release);

    const bool ok = dev_->start_streaming([this](const EEGSample& s) {
        // device callback thread
        if (!accepting_.load(std::memory_order_acquire)) return;

        // 1) core pipeline
        controller_->on_eeg_sample(s);

        // 2) GUI visualization ring
        const int nch = (int)s.channels.size();
        if (nch <= 0) return;

        {
            std::lock_guard<std::mutex> lk(vis_mu_);
            if (!vis_ready_) return;

            // If device reports a different channel count than expected:
            if (nch != vis_nch_) {
                // safest: re-init to the new channel count
                vis_nch_ = nch;
                vis_ring_.assign(vis_capacity_ * vis_nch_, 0.0f);
                vis_write_ = 0;
                ui_channel_ = 0;
            }

            float* dst = &vis_ring_[vis_write_ * vis_nch_];
            for (int c = 0; c < vis_nch_; ++c) dst[c] = s.channels[c];

            vis_write_ = (vis_write_ + 1) % vis_capacity_;
        }
    });


    if (!ok) {
        last_error_ = dev_->last_error();
        last_status_ = "start_streaming() failed";
        accepting_.store(false, std::memory_order_release);
        dev_->close();
        return false;
    }

    streaming_.store(true, std::memory_order_release);
    last_status_ = "Streaming started";
    last_error_.clear();
    return true;
}

void EEGGuiApp::stop_streaming_() {
    if (!streaming_.load(std::memory_order_acquire)) return;

    accepting_.store(false, std::memory_order_release);

    if (dev_) {
        dev_->stop_streaming();
        dev_->close();
    }

    streaming_.store(false, std::memory_order_release);
    last_status_ = "Streaming stopped";
}

// ----------------------
// UI
// ----------------------
void EEGGuiApp::draw_ui_() {
    // Fullscreen root window
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGui::Begin("MainLayout", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus
    );

    // ---------- layout params ----------
    const float pad = 10.0f;

    // Right column width (ratio-based)
    const float right_ratio = 0.33f;
    const float right_min_w = 360.0f;
    const float right_max_w = 520.0f;

    ImVec2 avail = ImGui::GetContentRegionAvail();

    float right_w = avail.x * right_ratio;
    if (right_w < right_min_w) right_w = right_min_w;
    if (right_w > right_max_w) right_w = right_max_w;

    float left_w = avail.x - right_w - pad;
    if (left_w < 300.0f) {
        left_w = 300.0f;
        right_w = avail.x - left_w - pad;
    }

    float full_h = avail.y;

    // ---------- LEFT : eeg_display ----------
    ImGui::BeginChild("eeg_display", ImVec2(left_w, full_h), true);
    ImGui::Text("eeg_display");
    ImGui::Separator();

    // Controls
    ImGui::Text("Waveform");
    ImGui::SliderInt("Channel", &ui_channel_, 0, std::max(0, vis_nch_ - 1));
    ImGui::SliderInt("Plot N", &ui_plot_n_, 100, std::min(1500, vis_capacity_));

    // Copy snapshot for this frame (avoid holding lock while plotting)
    std::vector<float> plot;
    int nch = 0;    
    int cap = 0;    
    int write = 0;  

    // Snapshot for this frame
    {
        std::lock_guard<std::mutex> lk(vis_mu_);
        if (vis_ready_ && !vis_ring_.empty() && vis_nch_ > 0) {
            const int N = std::min(ui_plot_n_, vis_capacity_);
            plot.resize(N);

            const int ch = std::min(ui_channel_, vis_nch_ - 1);
            int w = vis_write_;

            // newest at w-1, fill plot oldest->newest
            for (int i = 0; i < N; ++i) {
                int idx = w - N + i;
                while (idx < 0) idx += vis_capacity_;
                idx %= vis_capacity_;
                plot[i] = vis_ring_[idx * vis_nch_ + ch];
            }
        }
    }

    if (plot.empty()) {
        ImGui::TextDisabled("No samples yet...");
    } else {
        ImGui::PlotLines("EEG", plot.data(), (int)plot.size(), 0, nullptr,
                         FLT_MAX, FLT_MAX, ImVec2(0, 260));
    }

    ImGui::EndChild();

    ImGui::SameLine(0.0f, pad);

    // ---------- RIGHT : stacked panels ----------
    ImGui::BeginChild("right_col", ImVec2(right_w, full_h), false);

    // Fixed heights
    const float h_state   = 80.0f;
    const float h_eeg_btn = 95.0f;
    const float h_log     = 120.0f;  // 👈 eeg_log 줄임 (핵심)
    const float h_unity   = 120.0f;
    const float h_mode    = 110.0f;
    const float h_path    = 140.0f;

    // ----------------- 1) 상태창 -----------------
    ImGui::BeginChild("status_panel", ImVec2(0, h_state), true);
    ImGui::Text("status_panel");
    ImGui::Separator();
    ImGui::Text("EEG: %s", streaming_.load() ? "STREAMING" : "STOPPED");
    if (!last_status_.empty()) ImGui::Text("Message: %s", last_status_.c_str());
    if (!last_error_.empty())  ImGui::Text("Error: %s", last_error_.c_str());
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, pad));

    // ----------------- 2) eeg_button -----------------
    ImGui::BeginChild("eeg_button", ImVec2(0, h_eeg_btn), true);
    ImGui::Text("eeg_button");
    ImGui::Separator();

    if (!streaming_.load()) {
        if (ImGui::Button("Start", ImVec2(120, 0))) start_streaming_();
    } else {
        if (ImGui::Button("Stop", ImVec2(120, 0))) stop_streaming_();
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(120, 0))) want_quit_ = true;

    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, pad));

    // ----------------- 3) eeg_log (작게) -----------------
    ImGui::BeginChild("eeg_log", ImVec2(0, h_log), true);
    ImGui::Text("eeg_log");
    ImGui::Separator();
    ImGui::Text("lx_device_id: %d", lx_device_id_);
    ImGui::Text("numsample_return: %d", numsample_return_);
    ImGui::Text("num_channels: %d", num_channels_);
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, pad));

    // ----------------- 4) unity_log/button -----------------
    ImGui::BeginChild("unity_panel", ImVec2(0, h_unity), true);
    ImGui::Text("unity_log/button");
    ImGui::Separator();

    if (ImGui::Button("Ping Unity", ImVec2(140, 0))) {
        // TODO
    }
    ImGui::SameLine();
    if (ImGui::Button("Send SPACE_DOWN", ImVec2(170, 0))) {
        // TODO
    }
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, pad));

    // ----------------- 5) mode 설정 -----------------
    ImGui::BeginChild("mode_panel", ImVec2(0, h_mode), true);
    ImGui::Text("mode 설정 button");
    ImGui::Separator();

    static int mode = 0;
    ImGui::RadioButton("Idle", &mode, 0); ImGui::SameLine();
    ImGui::RadioButton("Record", &mode, 1); ImGui::SameLine();
    ImGui::RadioButton("Replay", &mode, 2);

    if (ImGui::Button("Apply Mode", ImVec2(140, 0))) {
        // TODO
    }
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, pad));

    // ----------------- 6) EEG 파일 경로 -----------------
    ImGui::BeginChild("path_panel", ImVec2(0, h_path), true);
    ImGui::Text("eeg data 파일이름/경로 설정");
    ImGui::Separator();

    static char out_dir[256]  = "./data";
    static char out_name[256] = "session_001";

    ImGui::InputText("Output dir", out_dir, IM_ARRAYSIZE(out_dir));
    ImGui::InputText("File name",  out_name, IM_ARRAYSIZE(out_name));

    if (ImGui::Button("Apply Path", ImVec2(140, 0))) {
        last_status_ = std::string("Output set: ") + out_dir + "/" + out_name;
    }
    ImGui::EndChild();

    ImGui::EndChild(); // right_col
    ImGui::End();      // MainLayout
}

// ----------------------
// Main loop
// ----------------------
int EEGGuiApp::run() {
    if (!init_window_and_imgui_()) {
        std::cerr << "[ERROR] GUI init failed: " << last_error_ << "\n";
        return 1;
    }

    while (!want_quit_) {
        if (glfwWindowShouldClose(impl_->window)) {
            want_quit_ = true;
        }

        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        draw_ui_();

        ImGui::Render();

        int display_w = 0, display_h = 0;
        glfwGetFramebufferSize(impl_->window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(impl_->window);
    }

    stop_streaming_();
    return 0;
}
