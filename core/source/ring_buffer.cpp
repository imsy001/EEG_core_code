#include "ring_buffer.hpp"

RingBuffer::RingBuffer(size_t capacity)
    : capacity_(capacity),
      buf_(capacity)  // capacity 만큼 미리 확보
{

}

void RingBuffer::push(const EEGSample& s) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (buf_.empty()) return;
    buf_[head_] = s;
    head_ = (head_ + 1) % capacity_;
    if (head_ == 0) filled_ = true;
}

std::vector<EEGSample> RingBuffer::snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<EEGSample> out;
    if (buf_.empty()) return out;

    if (!filled_) {
        out.assign(buf_.begin(), buf_.begin() + head_);
        return out;
    }

    out.reserve(capacity_);
    out.insert(out.end(), buf_.begin() + head_, buf_.end());
    out.insert(out.end(), buf_.begin(), buf_.begin() + head_);
    return out;
}

std::vector<EEGSample> RingBuffer::slice(double t0, double t1) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<EEGSample> out;
    if (buf_.empty()) return out;
    if (t1 < t0) return out;

    // Determine how many valid samples exist
    const size_t n = filled_ ? capacity_ : head_;
    if (n == 0) return out;

    out.reserve(n); // upper bound; we may push fewer

    // Iterate in chronological order:
    // If filled_: chronological starts at head_ (oldest) then wraps
    // If not filled_: chronological is [0 .. head_-1]
    auto emit_if_in_range = [&](const EEGSample& s) {
        const double ts = s.timestamp_sec;

        // Skip default/cleared samples (optional safeguard)
        if (ts == 0.0) return;

        if (ts < t0) return;
        if (ts > t1) return;
        out.push_back(s);
        };

    if (!filled_) {
        for (size_t i = 0; i < head_; ++i) {
            emit_if_in_range(buf_[i]);
        }
        return out;
    }

    // filled_ == true
    for (size_t i = 0; i < capacity_; ++i) {
        const size_t idx = (head_ + i) % capacity_;
        emit_if_in_range(buf_[idx]);
    }
    return out;
}


void RingBuffer::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    head_ = 0;
    filled_ = false;
    std::fill(buf_.begin(), buf_.end(), EEGSample{});
}
