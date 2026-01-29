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
        gui_error_ = "glfwInit() failed";
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
        gui_error_ = "glfwCreateWindow() failed";
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
        gui_error_ = "ImGui_ImplGlfw_InitForOpenGL() failed";
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init(impl_->glsl_version)) {
        gui_error_ = "ImGui_ImplOpenGL3_Init() failed";
        return false;
    }

    gui_status_ = "GUI initialized";
    gui_error_.clear();
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
        gui_status_ = "GUI: API already opened";
        gui_error_.clear();
        return true;
    }


#ifndef _WIN32
    gui_error_ = "OpenApi_LXDeviceAPI is Windows-only in this build.";
    gui_status_ = "API open failed";
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

    if (!dev_) {
        LXConfig cfg;
        cfg.lx_device_id = lx_device_id_;
        cfg.numsample_return = numsample_return_;
        cfg.num_channels = num_channels_;

        dev_ = make_lx_device(cfg);
        lsl_ = std::make_unique<DummyLSLBridge>();
        controller_ = std::make_unique<Controller>(*dev_, *lsl_);
    }

    gui_status_ = "GUI: API opened";
    gui_error_.clear();
    return true;

#endif
}

// ----------------------
// Device open/close
// ----------------------
bool EEGGuiApp::open_device_() {
    if (!open_api_()) return false;
    if (!controller_) {
        gui_error_ = "Controller not initialized (Open API failed?)";
        gui_status_.clear();
        return false;
    }
    controller_->post(Controller::CmdDeviceOpen{});
    gui_status_ = "GUI: Open Device requested";
    gui_error_.clear();
    return true;
}


bool EEGGuiApp::close_device_() {
    if (!open_api_()) return false;
    if (!controller_) {
        gui_error_ = "Controller not initialized (Open API failed?)";
        gui_status_.clear();
        return false;
    }
    controller_->post(Controller::CmdDeviceClose{});
    gui_status_ = "GUI: Close Device requested";
    gui_error_.clear();
    return true;
}


// ----------------------
// Streaming start/stop
// ----------------------
bool EEGGuiApp::start_streaming_() {
    if (!open_api_()) return false;
    if (!controller_) {
        gui_error_ = "Controller not initialized (Open API failed?)";
        gui_status_.clear();
        return false;
    }
    controller_->post(Controller::CmdStreamStart{});
    gui_status_ = "GUI: Start Streaming requested";
    gui_error_.clear();
    return true;
}


void EEGGuiApp::stop_streaming_() {
    gui_status_ = "GUI: Stop Streaming requested";
    gui_error_.clear();

    if (controller_) {
        controller_->post(Controller::CmdStreamStop{});
    }
    else {
        gui_error_ = "GUI Error: Controller not initialized (Open API first)";
        gui_status_.clear();
    }
}

// ----------------------
// UI
// ----------------------
void EEGGuiApp::draw_ui_() {
	// Pull stats from Controller
    Controller::Stats ctrl{};
    bool has_ctrl = false;

    if (controller_) {
        ctrl = controller_->stats();
        has_ctrl = true;
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

    const float h_state = 120.0f;
    const float h_eeg_btn = 95.0f;
    const float h_log = 120.0f;
    const float h_unity = 120.0f;
    const float h_mode = 110.0f;
    const float h_path = 140.0f;

    // ---------- status ----------
    ImGui::BeginChild("status_panel", ImVec2(0, h_state), true);
    ImGui::Text("Status");
    ImGui::Separator();

    // ---- GUI messages ----
    if (!gui_status_.empty())
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
            "GUI: %s", gui_status_.c_str());

    if (!gui_error_.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
            "GUI Error: %s", gui_error_.c_str());

    // ---- Controller truth ----
    if (has_ctrl) {
        if (!ctrl.last_status.empty())
            ImGui::Text("Core: %s", ctrl.last_status.c_str());

        if (!ctrl.last_error.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "Core Error: %s", ctrl.last_error.c_str());

        ImGui::Text("Core state: %s",
            controller_->is_streaming() ? "STREAMING" : "NOT STREAMING");
    }
    else {
        ImGui::TextDisabled("Core: not initialized");
    }


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
        if (!controller_) {
            gui_error_ = "GUI Error: Controller not initialized (Open API first)";
            gui_status_.clear();
        }
        else if (!controller_->is_streaming()) {
            gui_error_ = "Core is NOT STREAMING. Start streaming first.";
            gui_status_.clear();
        }
        else {
            const double ts = now_steady_seconds();

            // This triggers:
            // - epoch_.start(ts)
            // - fixed_epoch_end_ts_ = ts + 2.0
            // - fixed_epoch_armed_ = true
            // Then on_eeg_sample() will auto end at ts+2.0 and post CmdSaveEpoch/CmdInferEpoch
            controller_->on_marker(Marker::SPACE_DOWN, ts);

            gui_status_ = "GUI: Simulated SPACE_DOWN (will auto-save 2.0s epoch)";
            gui_error_.clear();
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
    ImGui::Text("mode setting button");
    ImGui::Separator();

    static int mode = 0;
    ImGui::RadioButton("Labeling Only", &mode, 0); ImGui::SameLine();
    ImGui::RadioButton("Inference Only", &mode, 1); ImGui::SameLine();
    ImGui::RadioButton("Inference and Labeling", &mode, 2);

    // mode: 0=Labeling Only, 1=Inference Only, 2=Inference and Labeling (your GUI)
    if (ImGui::Button("Apply Mode", ImVec2(140, 0))) {
        if (!controller_) {
            gui_error_ = "Controller not initialized (Open API first)";
            gui_status_.clear();
        }
        else if (mode == 0) {
            controller_->post(Controller::CmdSetRunningMode{
                Controller::RunningMode::LABELLING_ONLY });
            gui_status_ = "GUI: Mode set to LABELLING_ONLY";
            gui_error_.clear();
        }
        else if (mode == 1) {
            controller_->post(Controller::CmdSetRunningMode{
                Controller::RunningMode::INFERENCE_ONLY });
            gui_status_ = "GUI: Mode set to INFERENCE_ONLY";
            gui_error_.clear();
        }
        else {
            controller_->post(Controller::CmdSetRunningMode{
                Controller::RunningMode::INFERENCE_AND_LABELLING });
            gui_status_ = "GUI: Mode set to INFERENCE_AND_LABELLING";
            gui_error_.clear();
        }
    }


    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, pad));

    // ---------- path panel ----------
    ImGui::BeginChild("path_panel", ImVec2(0, h_path), true);
    ImGui::Text("eeg data filename/path setting");
    ImGui::Separator();

    static char out_dir[256] = "./data";
    static char out_name[256] = "session_001";

    ImGui::InputText("Output dir", out_dir, IM_ARRAYSIZE(out_dir));
    ImGui::InputText("File name", out_name, IM_ARRAYSIZE(out_name));

    if (ImGui::Button("Apply Path", ImVec2(140, 0))) {
        if (!controller_) {
            gui_error_ = "Controller not initialized (Open API first)";
            gui_status_.clear();
        }
        else {
            controller_->post(Controller::CmdSetOutputDir{ std::string(out_dir) });
            gui_status_ = std::string("GUI: Output dir set: ") + out_dir;
            gui_error_.clear();
        }
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
        std::cerr << "[ERROR] GUI init failed: " << gui_error_ << "\n";
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
