#pragma once
#include <vector>
#include <mutex>
#include "types.hpp"

class RingBuffer {
public:
    explicit RingBuffer(size_t capacity);

    void push(const EEGSample& s);
    std::vector<EEGSample> snapshot() const;

    // NEW: return samples with t0 <= timestamp <= t1 (chronological)
    std::vector<EEGSample> slice(double t0, double t1) const;

    void clear();

private:
    mutable std::mutex mtx_;
    size_t capacity_ = 0;
    std::vector<EEGSample> buf_;
    size_t head_ = 0;
    bool filled_ = false;
};


//👉 EEG에선 보통: capacity = sampling_rate × seconds //예: 1000 Hz × 5 sec = 5000 samples