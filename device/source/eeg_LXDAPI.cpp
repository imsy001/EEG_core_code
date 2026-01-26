/*LXDeviceAPI.cpp
Concrete EEGDevice implementation for LAXTHA (LXDeviceAPI SDK).

Device initialization

Start / stop acquisition

Raw sample callbacks

Timestamp handling

Thread safety at the device boundary*/

// source/eeg_LXAPI.cpp
//
// Concrete EEGDevice implementation for LAXTHA (LXDeviceAPI SDK).
// IMPORTANT:
//  - Include the vendor SDK headers ONLY in this .cpp (never in headers).
//  - Expose only the factory function (make_lx_device) via lx_device_factory.hpp.
//

#ifdef _WIN32
#include <windows.h>
#include "LXDeviceAPI.h"   // from LAXTHA SDK
#endif

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>
#include <cstring>
#include <algorithm>

#include "device/include/eeg_device.hpp"
#include "device/include/lx_device_factory.hpp"
#include "core/include/types.hpp"




// ===========================
// ✅ Vendor SDK headers here
// ===========================
#pragma comment(lib,"LXDeviceAPI.lib") // DLL implicit linking

#include "LXDeviceAPI.h"

namespace {

// ---------- time helper ----------
static double now_steady_seconds() {
    using namespace std::chrono;
    const auto t = steady_clock::now().time_since_epoch();
    return duration_cast<duration<double>>(t).count();
}

// ---------- LXDevice concrete implementation ----------
class LXDevice final : public EEGDevice {
public:
    explicit LXDevice(LXConfig cfg)
        : cfg_(std::move(cfg)) {}

    ~LXDevice() override {
        stop_streaming();
        close();
    }


    bool open() override {
    #ifndef _WIN32
        set_error_("LXDeviceAPI is Windows-only.");
        return false;
    #else
        // 0) quick checks under lock
        {
            std::lock_guard<std::mutex> lk(mu_);
            last_error_.clear();

            if (is_open_) return true;

            if (is_streaming_) {
                last_error_ = "open() called while streaming";
                return false;
            }
        }

        // 1) snapshot config outside lock (cfg_ isn't mutated after ctor in your design)
        const int lx_device_id = cfg_.lx_device_id;
        const int numsample = std::clamp(
            (cfg_.numsample_return > 0 ? cfg_.numsample_return : 32),
            1, 128
        );
        
        const int cfg_nch = (cfg_.num_channels > 0 ? cfg_.num_channels : 35);

        // 2) Open API
        //api_window = 0 : API 내장 UI 창 안 띄움
        //api_selfupdate = 0 : API가 알아서 업데이트 체크/창 띄우는 거 끔
        //mode = 0 : don't care(문서상 기본값 0)
        const int r_api = OpenApi_LXDeviceAPI(/*api_window=*/0, /*api_selfupdate=*/0, /*mode=*/0);
        if (r_api <= 0 && r_api != -2) {
            std::lock_guard<std::mutex> lk(mu_);
            last_error_ = "OpenApi_LXDeviceAPI failed: " + std::to_string(r_api);
            return false;
        }
        
        // record whether "we" opened the API (protect shared state)
        {
            std::lock_guard<std::mutex> lk(mu_);
            api_opened_by_me_ = (r_api > 0);   // -2 means "already open" => not by me
        }

        // 3) Open Device
        // ✅ IMPORTANT: pass &st_ (member), not a local stack variable
        int dhid_local = OpenDevice_LXDeviceAPI(
            /*LXDeviceID=*/lx_device_id,
            /*pSTStreamData=*/&st_,
            /*numsample_return=*/numsample,
            /*mode=*/0
        );

        if (dhid_local <= 0) {
            // Close API only if we opened it
            bool api_by_me = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                api_by_me = api_opened_by_me_;
            }
            if (api_by_me) CloseApi_LXDeviceAPI();

            std::lock_guard<std::mutex> lk(mu_);
            last_error_ = "OpenDevice_LXDeviceAPI failed: " + std::to_string(dhid_local);
            return false;
        }

        // 4) Query sample frequency (fallback allowed)
        int sample_rate_local = 500;
        {
            int sf = 0;
            const int r_sf = GetSampleFrequency_LXDeviceAPI(dhid_local, &sf);
            if (r_sf > 0 && sf > 0) sample_rate_local = sf;
        }

        // 5) Commit state under lock
        {
            std::lock_guard<std::mutex> lk(mu_);

            // If someone else opened concurrently (rare in your design), close this handle safely.
            if (is_open_) {
                CloseDevice_LXDeviceAPI(dhid_local);
                return true;
            }

            device_handling_id_ = dhid_local;
            numsample_return_   = numsample;

            info_.sample_rate_hz = sample_rate_local;
            info_.num_channels   = cfg_nch;
            info_.name           = "LAXTHA(LXDeviceAPI)";

            is_open_ = true;
            last_error_.clear();
        }

        return true;
    #endif
    }


    void close() override {
    #ifndef _WIN32
        // Windows 전용 SDK
        std::lock_guard<std::mutex> lk(mu_);
        is_open_ = false;
        is_streaming_ = false;
        cb_ = nullptr;
        last_error_.clear();
        return;
    #else
        // 0) 먼저 streaming이면 확실히 stop (락 밖에서 실행되도록 구성)
        //    stop_streaming() 내부에서 join까지 해주므로 close는 단순해짐.
        stop_streaming();

        // 1) 락으로 상태/핸들 스냅샷 후 "닫을 대상"만 가져오기
        int dhid_local = 0;
        bool api_by_me = false;

        {
            std::lock_guard<std::mutex> lk(mu_);

            // 이미 닫혔으면 끝
            if (!is_open_ && device_handling_id_ <= 0) {
                is_open_ = false;
                is_streaming_ = false;
                last_error_.clear();
                cb_ = nullptr;
                return;
            }

            // 스냅샷
            dhid_local = device_handling_id_;
            
            // 이 플래그는 너가 open()에서 관리해야 함:
            // - OpenApi 성공(>0)했으면 true
            // - -2(already open)면 false
            api_by_me = api_opened_by_me_;

            // 상태 먼저 내려두기 (이후 SDK 호출 중 재진입 방지)
            device_handling_id_ = 0;
            is_open_ = false;
            is_streaming_ = false;
            cb_ = nullptr;
            last_error_.clear();

        }

        // 2) SDK close 호출은 락 밖에서 (중요!)
        if (dhid_local > 0) {
            CloseDevice_LXDeviceAPI(dhid_local);
        }

        // 3) API 닫기: "내가 열었을 때만" 닫는 게 안전
        if (api_by_me) {
            CloseApi_LXDeviceAPI();
        }
    #endif
    }



    bool start_streaming(SampleCallback cb) override {
        // 0) 콜백 체크 (락 필요 없음)
        if (!cb) {
            set_error_("start_streaming: callback is empty");
            return false;
        }

        // 1) open()은 반드시 락 밖에서 (데드락/레이스 방지)
        if (!is_open() && !open()) return false;

        int dhid_local = 0;

        // 2) 상태 변경은 락 안에서
        {
            std::lock_guard<std::mutex> lk(mu_);
            last_error_.clear();

            if (is_streaming_) return true;

            cb_ = std::move(cb);
            stop_flag_.store(false, std::memory_order_release);

            dhid_local = device_handling_id_; // worker에서 쓸 스냅샷
            
            if (dhid_local <= 0) {
                last_error_ = "start_streaming: invalid device_handling_id_";
                cb_ = nullptr;
                return false;
            }
        }

    #ifdef _WIN32
        // 3) 메시지 루프용 스레드 시작
        worker_ = std::thread([this, dhid_local] {

            // worker thread 초반부에 추가
            MSG tmp;
            PeekMessage(&tmp, nullptr, 0, 0, PM_NOREMOVE);

            // 스레드 아이디 저장
            {
                std::lock_guard<std::mutex> lk(mu_);
                worker_tid_ = GetCurrentThreadId();
            }

            // --- hidden window 클래스 등록 ---
            WNDCLASSW wc{};
            wc.lpfnWndProc = &LXDevice::WndProcThunk;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"LXDeviceHiddenWindow_EEGCore";

            if (!RegisterClassW(&wc)) {
                const DWORD err = GetLastError();
                if (err != ERROR_CLASS_ALREADY_EXISTS) {
                    set_error_("RegisterClassW failed: " + std::to_string((int)err));
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        worker_tid_ = 0;
                    }
                    return;
                }
            }

        

        // --- hidden window 생성 ---
        HWND local_hwnd = CreateWindowExW(
            0, wc.lpszClassName, L"",
            0,
            0, 0, 0, 0,
            HWND_MESSAGE,
            nullptr,
            wc.hInstance,
            this
        );


        if (!local_hwnd) {
            const DWORD err = GetLastError();
            set_error_("CreateWindowEx failed: " + std::to_string((int)err));
            {
                std::lock_guard<std::mutex> lk(mu_);
                worker_tid_ = 0;   // ✅ 추가
            }
            return;
        }

        // hwnd_ 공유는 락으로 보호
        {
            std::lock_guard<std::mutex> lk(mu_);
            hwnd_ = local_hwnd;
        }

        // SDK에 메시지 등록
        const int r_msg = SetMessageDevice_LXDeviceAPI(
            dhid_local,
            MSGTYPEID0_DEVICE_LXDAPI,
            local_hwnd,
            msg_id_,
            1
        );
        
        if (r_msg <= 0) {
            set_error_("SetMessageDevice_LXDeviceAPI failed: " + std::to_string(r_msg));
            DestroyWindow(local_hwnd);
            {
                std::lock_guard<std::mutex> lk(mu_);
                hwnd_ = nullptr;
                worker_tid_ = 0;

            }
            return;
        }

        // 스트리밍 시작
        const int r_start = StartStream_LXDeviceAPI(dhid_local);
        if (r_start <= 0) {
            set_error_("StartStream_LXDeviceAPI failed: " + std::to_string(r_start));
            DestroyWindow(local_hwnd);
            {
                std::lock_guard<std::mutex> lk(mu_);
                hwnd_ = nullptr;
                worker_tid_ = 0;
            }
            return;
        }
        
        {
            std::lock_guard<std::mutex> lk(mu_);
            is_streaming_ = true;
        }

        // --- message loop ---
        MSG msg;
        while (!stop_flag_.load(std::memory_order_acquire) &&
               GetMessage(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // --- cleanup ---
        StopStream_LXDeviceAPI(dhid_local);
        DestroyWindow(local_hwnd);

        {
            std::lock_guard<std::mutex> lk(mu_);
            hwnd_ = nullptr;
            is_streaming_ = false;
            worker_tid_ = 0;
        }
    });

    #else
        set_error_("LXDeviceAPI streaming requires Windows");
        return false;
    #endif

        return true;
    }


    void stop_streaming() override {
    #ifdef _WIN32
        HWND hwnd_local = nullptr;
        DWORD tid_local = 0;
    #endif

        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!is_streaming_ && !worker_.joinable()) return;
            stop_flag_.store(true, std::memory_order_release); // ✅ 추가


    #ifdef _WIN32
            hwnd_local = hwnd_;
            tid_local = worker_tid_;
    #endif
        }

    #ifdef _WIN32
        // ✅ 제일 안정적인 방법: thread queue로 WM_QUIT 넣기
        if (tid_local != 0) {
            PostThreadMessage(tid_local, WM_QUIT, 0, 0);
        }

    #endif

        if (worker_.joinable()) worker_.join();

        {
            std::lock_guard<std::mutex> lk(mu_);
            is_streaming_ = false;
            cb_ = nullptr;
    #ifdef _WIN32
            worker_tid_ = 0;
            hwnd_ = nullptr;
    #endif
        }
    }


    DeviceInfo info() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return info_;
    }

    bool is_open() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return is_open_;
    }

    bool is_streaming() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return is_streaming_;
    }

    std::string last_error() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return last_error_;
    }

private:
    void set_error_(std::string msg) {
        std::lock_guard<std::mutex> lk(mu_);
        last_error_ = std::move(msg);
    }
    
private:
#ifdef _WIN32
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    DWORD worker_tid_ = 0;
    UINT msg_id_ = WM_USER + 1;
#endif


private:
    LXConfig cfg_;

    mutable std::mutex mu_;
    DeviceInfo info_{};
    SampleCallback cb_{};

    std::thread worker_;
    std::atomic<bool> stop_flag_{false};

    bool is_open_ = false;
    bool api_opened_by_me_ = false;
    bool is_streaming_ = false;
    std::string last_error_;

    int device_handling_id_ = 0;
    int numsample_return_ = 0;
    ST_STREAMDATA_LXDAPI st_{};

    // TODO: store vendor handle here
    // -------------------------
    // LXHandle* handle_ = nullptr;
};


#ifdef _WIN32
LRESULT CALLBACK LXDevice::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        auto* self = reinterpret_cast<LXDevice*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }

    auto* self = reinterpret_cast<LXDevice*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (self) {
        return self->WndProc(hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT LXDevice::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg != msg_id_) {
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    const unsigned int u = (unsigned int)wParam;

    const unsigned char msgtype_subid = (unsigned char)(u & 0xFF);
    const unsigned char msgtype_id    = (unsigned char)((u >> 8) & 0xFF);
    const unsigned short dhid         = (unsigned short)((u >> 16) & 0xFFFF);

    if (!(msgtype_id == MSGTYPEID0_DEVICE_LXDAPI && msgtype_subid == 0)) return 0;

    SampleCallback cb_local;
    int nch = 0, N = 0, sr = 0;
    unsigned int expected = 0;

    {
        std::lock_guard<std::mutex> lk(mu_);
        N = numsample_return_;
        sr = info_.sample_rate_hz;
        cb_local = cb_;
        nch = info_.num_channels;
        expected = (unsigned int)device_handling_id_;
    }

    if ((unsigned int)dhid != expected) return 0;
    if (!cb_local || nch <= 0 || sr <= 0 || N <= 0) return 0;

    if (GetStreamData_LXDeviceAPI(u) <= 0) return 0;
    if (!st_.Wave_StreamData_CS) return 0;

    const double t0 = now_steady_seconds();
    const double dt = 1.0 / double(sr);

    for (int i = 0; i < N; ++i) {
        EEGSample s;
        s.timestamp_sec = t0 + i * dt;

        s.channels.resize(nch);
        for (int ch = 0; ch < nch; ++ch) {
            s.channels[ch] = st_.Wave_StreamData_CS[i + N * ch];
        }

        cb_local(s);
    }

    return 0;
}
#endif
} // namespace


// Factory function (declared in header/lx_device_factory.hpp)
std::unique_ptr<EEGDevice> make_lx_device(const LXConfig& cfg) {
    return std::make_unique<LXDevice>(cfg);
}


/*
TODO sections to implement with real SDK calls:

open/init + query sample rate/channels

start/stop streaming (if needed)

read samples (polling or SDK callback)

If you paste the exact API calls you found in the LAXTH repo 
(the open/start/read functions or an example snippet), 
I’ll map them into the TODO spots precisely so this compiles and streams real data.*/

/*
make_lx_device()
    ↓
open()
    ↓
start_streaming(cb)
    ↓
[worker thread]
    stream_loop_()
        → cb_(EEGSample)
    ↓
stop_streaming()
    ↓
close()
*/

/*✅ 3) GetStreamData_LXDeviceAPI(u)가 st_를 채우는 방식이 맞는지 확인 필요 (가장 중요)

너 open()에서 이렇게 했어:

OpenDevice_LXDeviceAPI(..., &st_, numsample, 0);


즉, “SDK가 st_에 버퍼 포인터(Wave_StreamData_CS 등)를 채워준다”라는 전제를 깔고 있어.

그리고 WndProc에서 이렇게 쓰지:

if (!st_.Wave_StreamData_CS) return 0;
s.channels[ch] = st_.Wave_StreamData_CS[i + N * ch];


✅ 이게 SDK 설계가 그 방식이면 완벽이야.

⚠️ 근데 만약 SDK가 GetStreamData_LXDeviceAPI(u) 호출 후에

내부 전역 버퍼를 갱신하고

st_는 그냥 “설정 구조체”로만 쓰는 방식이면
이 접근이 틀릴 수 있어.

👉 이건 실제 문서/예제에서 st_.Wave_StreamData_CS를 직접 읽는 예제가 있는지만 확인하면 끝이야.

(지금은 네가 &st_를 넘긴 설계이므로 “맞을 가능성”이 높아 보임.)*/

