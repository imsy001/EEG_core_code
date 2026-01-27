// If your project expects LXDAPI device factory, provide a stub factory here.
// The goal is: compile + run GUI/core without real hardware.

#include "lx_device_factory.hpp"   // or the header where it's declared
#include "eeg_device.hpp"
#include <memory>
#include <string>

namespace {

// A tiny stub device that compiles on mac
class DummyLXDevice final : public EEGDevice {
public:
    bool open() override { last_err_ = "LXDeviceAPI is Windows-only."; return false; }
    void close() override {}

    bool start_streaming(SampleCallback cb) override { (void)cb; last_err_ = "Not supported on macOS."; return false; }
    void stop_streaming() override {}

    DeviceInfo info() const override { return {}; }
    bool is_open() const override { return false; }
    bool is_streaming() const override { return false; }
    std::string last_error() const override { return last_err_; }

private:
    std::string last_err_;
};

} // namespace

std::unique_ptr<EEGDevice> make_lx_device(const LXConfig& cfg) {
    (void)cfg;
    return std::make_unique<DummyLXDevice>();
}
