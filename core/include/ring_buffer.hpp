#pragma once
#include <vector>
#include <mutex>
#include "types.hpp"

class RingBuffer {
public:
    explicit RingBuffer(size_t capacity);

    void push(const EEGSample& s);          // callback thread
    std::vector<EEGSample> snapshot() const; // GUI thread

    void clear();

private:
    size_t capacity_;
    mutable std::mutex mtx_;
    std::vector<EEGSample> buf_;
    size_t head_ = 0;
    bool filled_ = false;
};

//👉 EEG에선 보통: capacity = sampling_rate × seconds //예: 1000 Hz × 5 sec = 5000 samples