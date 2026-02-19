#include "eeg_gui.hpp"

#include <iostream>
#include <utility>
#include <algorithm>
#include <vector>
#include <chrono>
#include <cstring>

#include "unity_LSL/include/lsl_bridge_impl.hpp"
#include "inference/include/inference.hpp"




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

#ifdef __APPLE__
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
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
        lsl_ = std::make_unique<LSLBridgeImpl>(
            "UnityMarkers",          // ✅ RX (Unity -> C++)
            "CppToUnityMarkers",     // ✅ TX marker
            "CppToUnityDirections"   // ✅ TX direction
        );

        controller_ = std::make_unique<Controller>(*dev_, *lsl_);

        // IMPORTANT: set infer callback once; it will use onnx_ if loaded
        controller_->set_infer_fn([this](const std::vector<EEGSample>& epoch, const EpochMeta& meta)
            -> std::optional<Direction>
            {
                if (!onnx_) return std::nullopt; // model not loaded yet
                try {
                    return onnx_->infer_direction(epoch);
                }
                catch (const std::exception& e) {
                    // optional: surface error
                    // gui_error_ = std::string("ONNX infer error: ") + e.what(); // careful: UI thread safety
                    return std::nullopt;
                }
            });
    }

    ::OpenApi_LXDeviceAPI(1, 0, 0);
    api_opened_ = true;

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

    const float right_ratio = 0.25f;
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
    const int K = 27;
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
        // ---- Controls (fixed, not scrolling) ----
        static float eeg_scale = 100.0f;
        ImGui::SliderFloat("EEG scale (±uV)", &eeg_scale, 10.0f, 500.0f);

        // (optional) tighter spacing to fit more per screen
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2.0f));

        // Remaining height for scrolling area
        float scroll_h = ImGui::GetContentRegionAvail().y;

        // ---- Scrollable plots area ----
        ImGui::BeginChild("plot_scroll", ImVec2(0.0f, scroll_h), true,
            ImGuiWindowFlags_HorizontalScrollbar);

        // Decide per-plot height (fixed height per channel)
        static float per_plot_h = 55.0f;                 // ✅ 이 값을 키우면 "세로 커짐"
        ImGui::SliderFloat("Plot height", &per_plot_h, 20.0f, 160.0f);
        ImGui::Separator();

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
                ImVec2(0.0f, per_plot_h) // ✅ 고정 height -> 스크롤로 전체를 볼 수 있음
            );

            ImGui::PopID();
        }

        ImGui::EndChild(); // plot_scroll
        ImGui::PopStyleVar();
    }



    ImGui::EndChild();
    ImGui::SameLine(0.0f, pad);


    // =========================================================
// RIGHT : CONTROL PANELS
// =========================================================
    ImGui::BeginChild("right_col", ImVec2(right_w, full_h), false);

    // ---- panel heights ----
    const float h_state = 90.0f;
    const float h_check = 170.0f;
    const float h_control = 60.0f;     // Open API / Open Device
    const float h_eeg_btn = 150.0f;    // Start/Stop + 3 sim buttons
    const float h_unity = 260.0f;
    const float h_mode = 150.0f;
    const float h_path = 110.0f;
    const float h_infer = 120.0f;

    // ---------- Status ----------
    ImGui::BeginChild("status_panel", ImVec2(0, h_state), true);
    ImGui::Text("Status");
    ImGui::Separator();

    // GUI messages
    if (!gui_status_.empty())
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
            "GUI: %s", gui_status_.c_str());

    if (!gui_error_.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
            "GUI Error: %s", gui_error_.c_str());

    // Controller truth
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


    // ---------- Checklist ----------
    ImGui::BeginChild("checklist_panel", ImVec2(0, h_check), true);
    ImGui::Text("Checklist");
    ImGui::Separator();

    // Persist checkbox states across frames
    static bool ck_open_api = false;
    static bool ck_open_dev = false;
    static bool ck_lsl_conn = false;     // (기존 ck_lsl_conn 대체)
    static bool ck_mode_applied = false;   // ✅ 추가

    

    // Auto-fill (read-only처럼 쓰고 싶으면 아래 Text로 바꾸는 것도 가능)
    ck_open_api = api_opened_;
    ck_lsl_conn = lsl_ ? lsl_->rx_running() : false;

    // ck_open_dev는 네가 실제 device-open 상태 플래그가 없어서 일단 수동 유지.
    // (원하면 Controller::Stats에 device_opened 같은 거 추가해서 여기 자동화 가능)
    ImGui::Checkbox("Open API", &ck_open_api);
    ImGui::Checkbox("Open Device", &ck_open_dev);
    ImGui::Checkbox("Unity Connected", &ck_lsl_conn);
    ImGui::Checkbox("Mode applied", &ck_mode_applied);
    

    int done = (int)ck_open_api + (int)ck_open_dev + (int)ck_lsl_conn;
    ImGui::Separator();
    ImGui::Text("Progress: %d / 4", done);

    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, pad));


    // ---------- Control (API / Device) ----------
    ImGui::BeginChild("control_panel", ImVec2(0, h_control), true);
    ImGui::Text("Control");
    ImGui::Separator();

    {
        float w = ImGui::GetContentRegionAvail().x;
        float gap = ImGui::GetStyle().ItemSpacing.x;
        float half = (w - gap) * 0.5f;

        // Open API
        ImGui::BeginDisabled(api_opened_);
        if (ImGui::Button("Open API", ImVec2(half, 0))) {
            open_api_();
            // Checklist sync (optional)
            ck_open_api = api_opened_;
        }
        ImGui::EndDisabled();

        ImGui::SameLine(0.0f, gap);

        // Open Device (API must be opened)
        ImGui::BeginDisabled(!api_opened_);
        if (ImGui::Button("Open Device", ImVec2(half, 0))) {
            open_device_();
            ck_open_dev = true; // (임시) 실제 open 확인은 core stats로 바꾸는게 베스트
        }
        ImGui::EndDisabled();
    }

    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, pad));


    // ---------- Unity panel ----------
    ImGui::BeginChild("unity_panel", ImVec2(0, h_unity), true);
    ImGui::Text("unity_log/button");
    ImGui::Separator();

    ImGui::Text("RX: UnityMarkers | TX: CppToUnityMarkers");
    const bool reader_running = lsl_ ? lsl_->rx_running() : false;

    if (!lsl_) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1),
            "LSLBridge not initialized (Open API first)");
    }
    else {
        if (!reader_running) {
            if (ImGui::Button("Connect", ImVec2(140, 0))) {
                if (!controller_) {
                    gui_error_ = "Controller not initialized (Open API first)";
                    gui_status_.clear();
                }
                else {
                    lsl_->start_rx([&](const std::string& text, double lsl_ts) {
                        controller_->on_marker_text(text, lsl_ts);
                        });
                    log_rx_.push("[RX] Connecting to stream: UnityMarkers");
                }
            }
        }
        else {
            if (ImGui::Button("Stop", ImVec2(140, 0))) {
                lsl_->stop_rx();
                log_rx_.push("[RX] Stopped");
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear Logs", ImVec2(140, 0))) {
        log_rx_.clear();
        log_tx_.clear();
        log_core_.clear(); // ✅ 코어도 같이 지우고 싶으면
    }

    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &unity_auto_scroll_);

    ImGui::Separator();
    ImGui::Text("Core log");

    // unity_panel 내부에 core_log_view 넣기
    ImGui::BeginChild("core_log_view", ImVec2(0, 120), true);
    auto lines = log_core_.snapshot();
    for (const auto& line : lines)
        ImGui::TextUnformatted(line.c_str());
    if (unity_auto_scroll_) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::EndChild();              // ✅ unity_panel 끝 (딱 1번)
    ImGui::Dummy(ImVec2(0, pad));



    // ---------- Mode panel ----------
    ImGui::BeginChild("mode_panel", ImVec2(0, h_mode), true);
    ImGui::Text("Mode setting");
    ImGui::Separator();

    static bool mode_labeling = false;
    static bool mode_inference = false;

    ImGui::Checkbox("Labeling", &mode_labeling);
    ImGui::SameLine();
    ImGui::Checkbox("Inference", &mode_inference);
    
    ImGui::Separator();

    // 현재 선택 상태 표시 (가독성)
    const char* mode_text = "NONE";
    if (mode_labeling && mode_inference) mode_text = "LABELING + INFERENCE";
    else if (mode_labeling)             mode_text = "LABELING ONLY";
    else if (mode_inference)            mode_text = "INFERENCE ONLY";

    ImGui::Text("Current: %s", mode_text);

    // Apply 버튼
    if (ImGui::Button("Apply Mode", ImVec2(140, 0))) {
        if (!controller_) {
            gui_error_ = "Controller not initialized (Open API first)";
            gui_status_.clear();
        }
        else {
            bool applied = false;

            if (mode_labeling && mode_inference) {
                controller_->post(Controller::CmdSetRunningMode{ Controller::RunningMode::INFERENCE_AND_LABELLING });
                gui_status_ = "GUI: Mode set to INFERENCE_AND_LABELLING";
                applied = true;
            }
            else if (mode_labeling) {
                controller_->post(Controller::CmdSetRunningMode{ Controller::RunningMode::LABELLING_ONLY });
                gui_status_ = "GUI: Mode set to LABELLING_ONLY";
                applied = true;
            }
            else if (mode_inference) {
                controller_->post(Controller::CmdSetRunningMode{ Controller::RunningMode::INFERENCE_ONLY });
                gui_status_ = "GUI: Mode set to INFERENCE_ONLY";
                applied = true;
            }
            else {
                gui_status_ = "GUI: Mode unchanged (none selected)";
            }

            gui_error_.clear();

            if (applied) ck_mode_applied = true;
        }
    }


    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, pad));



    // ---------- Path panel ----------
    ImGui::BeginDisabled(!mode_labeling);   // 🔴 Labeling 아닐 때 비활성화

    ImGui::BeginChild("path_panel", ImVec2(0, h_path), true);
    ImGui::Text("eeg data filename/path setting");
    ImGui::Separator();

    static char out_dir[256] = "./data";
    
    ImGui::InputText("Output dir", out_dir, IM_ARRAYSIZE(out_dir));
    
    if (ImGui::Button("Apply Path", ImVec2(140, 0))) {
        if (!controller_) {
            gui_error_ = "Controller not initialized (Open API first)";
            gui_status_.clear();
        }
        else {
            std::string full_dir = std::string(out_dir);
            if (full_dir.back() != '/' && full_dir.back() != '\\') full_dir += "/";
            

            controller_->post(Controller::CmdSetOutputDir{ full_dir });

            gui_status_ = "GUI: Output dir set: " + full_dir;
            gui_error_.clear();
        }
    }


    ImGui::EndChild();

    ImGui::EndDisabled();                   // 🔴 여기까지
    ImGui::Dummy(ImVec2(0, pad));


    // ---------- Inference model panel ----------
    ImGui::BeginDisabled(!mode_inference);
    ImGui::BeginChild("inference_model_panel", ImVec2(0, h_infer), true);
    ImGui::Text("Inference model");
    ImGui::Separator();

    static char model_path[512] = "./model/best_model.onnx";
    ImGui::InputText("ONNX path", model_path, IM_ARRAYSIZE(model_path));

    const bool core_ready = (controller_ != nullptr);
    const bool streaming_now = core_ready ? controller_->is_streaming() : false;

    ImGui::Text("Ready: %s", core_ready ? "YES" : "NO");
    ImGui::Text("Streaming: %s", streaming_now ? "YES" : "NO");

    ImGui::Separator();

    ImGui::BeginDisabled(!core_ready);

    if (ImGui::Button("Load Model", ImVec2(140, 0))) {
        try {
            std::wstring wpath(model_path, model_path + std::strlen(model_path));
            onnx_ = std::make_unique<eeg::OnnxInfer>(wpath);

            gui_status_ = "GUI: ONNX model loaded";
            gui_error_.clear();
        }
        catch (const std::exception& e) {
            onnx_.reset();
            gui_error_ = std::string("GUI: ONNX load failed: ") + e.what();
            gui_status_.clear();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Run Inference Now", ImVec2(180, 0))) {
        gui_status_ = "GUI: Run Inference (TODO)";
        gui_error_.clear();
    }

    ImGui::EndDisabled();

    ImGui::EndChild();
    ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0, pad));



    // ---------- EEG control (moved to bottom) ----------
    ImGui::BeginChild("eeg_button", ImVec2(0, h_eeg_btn), true);
    ImGui::Text("EEG control");
    ImGui::Separator();

    const bool isStreaming = controller_ ? controller_->is_streaming() : false;

    if (!isStreaming) {
        ImGui::BeginDisabled(!api_opened_);
        if (ImGui::Button("Start Streaming", ImVec2(180, 0))) {
            start_streaming_();
        }
        ImGui::EndDisabled();
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

    if (ImGui::Button("Sim SPACE_DOWN (save after 2s)", ImVec2(260, 0))) {
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
            controller_->on_marker(Marker::SPACE_DOWN, ts);
            gui_status_ = "GUI: Simulated SPACE_DOWN (will auto-save 2.0s epoch)";
            gui_error_.clear();
            
        }
    }

    if (ImGui::Button("Sim SPACE_DOWN (save before 2s)", ImVec2(260, 0))) {
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
            controller_->on_marker_pre(Marker::SPACE_DOWN, ts);
            gui_status_ = "GUI: Simulated SPACE_DOWN (saved 2s pre-trigger epoch)";
            gui_error_.clear();
            
        }
    }

    if (ImGui::Button("Sim SPACE_DOWN (save before and after 2s)", ImVec2(260, 0))) {
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
            controller_->on_marker_pre_post(Marker::SPACE_DOWN, ts);
            gui_status_ = "GUI: Simulated SPACE_DOWN (saved 2s pre + 2s post)";
            gui_error_.clear();
            
        }
    }

    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, pad));


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

