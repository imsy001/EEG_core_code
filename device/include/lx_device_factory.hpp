#pragma once

#include <memory>
#include <string>

class EEGDevice;   // forward declaration

// LAXTH-specific configuration only
struct LXConfig {
    // --- Device selection ---
    int lx_device_id = 0;     
    // 예:
    // 300    = QEEG-32FX
    // 16432  = QEEG-64FX (32ch option)
    // (LXDeviceAPI 문서 Appendix 기준)

    // --- Stream configuration ---
    int numsample_return = 32;
    // SDK가 한 번에 메시지로 알려주는 sample block 크기
    // (보통 16 / 32 / 64 추천)

    int num_channels = 0;
    // 실제 사용할 채널 수
    // (EEG32 + EOG2 + ECG1 = 35 등)
    // 0이면 device preset 기준으로 자동 설정

    // --- Timing / behavior ---
    bool use_event_channel = false;
    // Event_StreamData_CS[] 사용할지 여부
    // (나중에 LX 내부 event marking 쓸 때)

    // --- Debug / safety ---
    bool verbose = false;
    // SDK return code, stream 상태 로그 출력용
};

std::unique_ptr<EEGDevice> make_lx_device(const LXConfig& cfg);
