#include "epoch_buffer.hpp"
#include <utility>           // std::move

void EpochBuffer::start(double ts) {
    std::lock_guard<std::mutex> lk(mtx_);
    buf_.clear();
    start_ts_ = ts;
    active_ = true;
}

void EpochBuffer::push(const EEGSample& s) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!active_) return;
    buf_.push_back(s);
}

bool EpochBuffer::active() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return active_;
}

std::vector<EEGSample> EpochBuffer::end(double ts, double min_dur_sec) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!active_) return {};

    active_ = false;

    const double dur = ts - start_ts_;
    if (dur < min_dur_sec) {
        buf_.clear();
        return {};
    }

    auto out = std::move(buf_);
    buf_.clear();
    return out;
}

void EpochBuffer::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    active_ = false;
    start_ts_ = 0.0;
    buf_.clear();
}
