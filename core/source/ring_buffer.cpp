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

void RingBuffer::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    head_ = 0;
    filled_ = false;
    std::fill(buf_.begin(), buf_.end(), EEGSample{});
}
