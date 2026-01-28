#include "eeg_gui.hpp"

#include <iostream>
#include <utility>
#include <algorithm>
#include <vector>
#include <chrono>

static double now_steady_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32
#include "LXDeviceAPI.h" // for OpenApi_LXDeviceAPI()
#endif

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
    : impl_(std::make_unique<Impl>()) {
}

EEGGuiApp::~EEGGuiApp() {
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
    last_error_.clear();
    return true;
}

void EEGGuiApp::shutdown_window_and_imgui_() {
    if (impl_ && impl_->window) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        glfwDestroyWindow(impl_->window);
        impl_->window = nullptr;

        glfwTerminate();
    }
}

// ----------------------
// API open
// ----------------------
bool EEGGuiApp::open_api_() {
    if (api_opened_) {
        last_status_ = "API already opened";
        return true;
    }

#ifndef _WIN32
    last_error_ = "OpenApi_LXDeviceAPI is Windows-only in this build.";
    last_status_ = "API open failed";
    return false;
#else
    // Build device + controller once
    if (!dev_) {
        LXConfig cfg;
        cfg.lx_device_id = lx_device_id_;
        cfg.numsample_return = numsample_return_;
        cfg.num_channels = num_channels_;

        dev_ = make_lx_device(cfg);
        lsl_ = std::make_unique<DummyLSLBridge>();
        controller_ = std::make_unique<Controller>(*dev_, *lsl_);
    }

    ::OpenApi_LXDeviceAPI(1, 0, 0);
    api_opened_ = true;
    last_status_ = "API opened";
    last_error_.clear();
    return true;
#endif
}

// ----------------------
// Device open/close
// ----------------------
bool EEGGuiApp::open_device_() {
	last_error_.clear();
	last_status_ = "Opening device...";
    controller_->post(Controller::CmdDeviceOpen{});
    return true;
}

bool EEGGuiApp::close_device_() {
    last_error_.clear();
    last_status_ = "Closing device...";
    controller_->post(Controller::CmdDeviceClose{});
    return true;
}

// ----------------------
// Streaming start/stop
// ----------------------
bool EEGGuiApp::start_streaming_() {
    
    last_error_.clear();
    last_status_ = "Starting streaming...";
    controller_->post(Controller::CmdStreamStart{});   
    return true;
}

void EEGGuiApp::stop_streaming_() {
    last_error_.clear();
    last_status_ = "Stopping streaming...";
    if (controller_) {
        controller_->post(Controller::CmdStreamStop{});
    }
    else {
        last_error_ = "Controller not initialized";
        last_status_ = "Stop failed";
    }
}

// ----------------------
// UI
// ----------------------
void EEGGuiApp::draw_ui_() {

    if (controller_) {
        const auto st = controller_->stats();  // you already have stats_ + mutex
        last_status_ = st.last_status;
        last_error_ = st.last_error;
    }

    // Fullscreen root window
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGui::Begin(
        "MainLayout",
        nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus
    );

    // ---------- layout params ----------
    const float pad = 10.0f;

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
    // =========================================================
    // LEFT : EEG DISPLAY
    //   - Uses Controller-owned visualization ring buffer via vis_snapshot()
    // =========================================================
    ImGui::BeginChild("eeg_display", ImVec2(left_w, full_h), true);
    ImGui::Text("eeg_display");
    ImGui::Separator();

    ImGui::Text("Waveform");

    // Pull a snapshot from Controller (thread-safe copy)
    Controller::VisSnapshot snap{};
    if (controller_) {
        snap = controller_->vis_snapshot();
    }


    // Slider max should follow snapshot capacity (fallback to 1500 if empty)
    const int maxPlotN = (snap.capacity > 0) ? std::min(1500, snap.capacity) : 1500;
    ImGui::SliderInt("Plot N", &ui_plot_n_, 100, maxPlotN);

    // ---- Multi-channel (first K) stacked plots ----
    const int K = 6;
    std::vector<float> plots;
    int N = 0;
    int useK = 0;

    // Build plot buffers from snapshot
    if (snap.nch > 0 && snap.capacity > 0 && !snap.ring.empty()) {
        N = std::min(ui_plot_n_, snap.capacity);
        useK = std::min(K, snap.nch);

        plots.assign(useK * N, 0.0f);

        const int w = snap.write;

        for (int i = 0; i < N; ++i) {
            int idx = w - N + i;
            while (idx < 0) idx += snap.capacity;
            idx %= snap.capacity;

            const float* src = &snap.ring[idx * snap.nch];
            for (int c = 0; c < useK; ++c) {
                plots[c * N + i] = src[c];
            }
        }
    }

    if (plots.empty()) {
        ImGui::TextDisabled("No samples yet...");
    }
    else {
        float avail_h = ImGui::GetContentRegionAvail().y;
        float per_h = std::max(50.0f, (avail_h - 20.0f) / (float)useK);

        // One shared scale slider (not repeated per channel)
        static float eeg_scale = 100.0f;
        ImGui::SliderFloat("EEG scale (±uV)", &eeg_scale, 10.0f, 500.0f);

        for (int c = 0; c < useK; ++c) {
            ImGui::PushID(c);
            ImGui::Text("Ch %d", c);

            ImGui::PlotLines(
                "##EEG",
                &plots[c * N],
                N,
                0,
                nullptr,
                -eeg_scale,
                eeg_scale,
                ImVec2(0, per_h)
            );

            ImGui::PopID();
        }
    }

    ImGui::EndChild();
    ImGui::SameLine(0.0f, pad);


    // =========================================================
    // RIGHT : CONTROL PANELS
    // =========================================================
    ImGui::BeginChild("right_col", ImVec2(right_w, full_h), false);

    const float h_state = 80.0f;
    const float h_eeg_btn = 95.0f;
    const float h_log = 120.0f;
    const float h_unity = 120.0f;
    const float h_mode = 110.0f;
    const float h_path = 140.0f;

    // ---------- status ----------
    ImGui::BeginChild("status_panel", ImVec2(0, h_state), true);
    ImGui::Text("status_panel");
    ImGui::Separator();
    ImGui::Text("EEG: %s", (controller_ ? "READY" : "NOT INITIALIZED"));
    if (!last_status_.empty()) ImGui::Text("Message: %s", last_status_.c_str());
    if (!last_error_.empty())  ImGui::Text("Error: %s", last_error_.c_str());
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, pad));

    // ---------- EEG control ----------
    ImGui::BeginChild("eeg_button", ImVec2(0, h_eeg_btn), true);
    ImGui::Text("EEG control");
    ImGui::Separator();

    if (ImGui::Button("Open API", ImVec2(140, 0))) {
        open_api_();
    }
    if (ImGui::Button("Open Device", ImVec2(140, 0))) {
        open_device_();
    }

    const bool isStreaming = controller_ ? controller_->is_streaming() : false;

    if (!isStreaming) {
        if (ImGui::Button("Start Streaming", ImVec2(180, 0))) {
            start_streaming_();
        }
    }
    else {
        if (ImGui::Button("Stop Streaming", ImVec2(180, 0))) {
            stop_streaming_();
        }
    }


    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(120, 0))) {
        want_quit_ = true;
    }

    ImGui::Separator();

    if (ImGui::Button("Sim SPACE_DOWN (save 2s)", ImVec2(220, 0))) {
        if (controller_) {
            const double ts = now_steady_seconds();
            controller_->on_marker(Marker::SPACE_DOWN, ts);
            last_status_ = "Simulated SPACE_DOWN: will save 2s epoch";
        }
        else {
            last_error_ = "Controller not initialized";
        }
    }

    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, pad));

    // ---------- eeg log ----------
    ImGui::BeginChild("eeg_log", ImVec2(0, h_log), true);
    ImGui::Text("eeg_log");
    ImGui::Separator();
    ImGui::Text("lx_device_id: %d", lx_device_id_);
    ImGui::Text("numsample_return: %d", numsample_return_);
    ImGui::Text("num_channels: %d", num_channels_);
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, pad));

    // ---------- unity panel ----------
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

    // ---------- mode panel ----------
    ImGui::BeginChild("mode_panel", ImVec2(0, h_mode), true);
    ImGui::Text("mode 설정 button");
    ImGui::Separator();

    static int mode = 0;
    ImGui::RadioButton("Idle", &mode, 0); ImGui::SameLine();
    ImGui::RadioButton("Record", &mode, 1); ImGui::SameLine();
    ImGui::RadioButton("Replay", &mode, 2);

    // mode: 0=Idle, 1=Record, 2=Replay (your GUI)
    if (ImGui::Button("Apply Mode", ImVec2(140, 0))) {
        if (mode == 1) {
            controller_->post(Controller::CmdSetRunningMode{ Controller::RunningMode::LABELLING_ONLY });
            controller_->post(Controller::CmdArmRecording{ true });
            last_status_ = "Mode: LABELLING_ONLY (saving enabled)";
        }
        else {
            controller_->post(Controller::CmdSetRunningMode{ Controller::RunningMode::INFERENCE_ONLY });
            controller_->post(Controller::CmdArmRecording{ false });
            last_status_ = "Mode: INFERENCE_ONLY (saving disabled)";
        }
    }

    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, pad));

    // ---------- path panel ----------
    ImGui::BeginChild("path_panel", ImVec2(0, h_path), true);
    ImGui::Text("eeg data 파일이름/경로 설정");
    ImGui::Separator();

    static char out_dir[256] = "./data";
    static char out_name[256] = "session_001";

    ImGui::InputText("Output dir", out_dir, IM_ARRAYSIZE(out_dir));
    ImGui::InputText("File name", out_name, IM_ARRAYSIZE(out_name));

    if (ImGui::Button("Apply Path", ImVec2(140, 0))) {
        controller_->post(Controller::CmdSetOutputDir{ std::string(out_dir) });
        last_status_ = std::string("Output dir set: ") + out_dir;
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
