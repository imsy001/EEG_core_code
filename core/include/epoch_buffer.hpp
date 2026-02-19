#pragma once
#include <vector>
#include <mutex>
#include "types.hpp"

class EpochBuffer {
public:
    void start(double ts);
    void push(const EEGSample& s);
    bool active() const;

    // epoch 종료 시 데이터 이동
    std::vector<EEGSample> end(double ts, double min_dur_sec);

    void clear();
    void seed(double start_ts, std::vector<EEGSample>&& pre);

private:
    mutable std::mutex mtx_;
    bool active_ = false;
    double start_ts_ = 0.0;
    std::vector<EEGSample> buf_;
};
