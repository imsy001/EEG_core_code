#pragma once
#include <deque>
#include <mutex>
#include <string>
#include <vector>

struct LogBuffer {
    std::mutex mu;
    std::deque<std::string> lines;
    size_t max_lines = 2000;

    void push(const std::string& s) {
        std::lock_guard<std::mutex> lk(mu);
        lines.push_back(s);
        while (lines.size() > max_lines)
            lines.pop_front();
    }

    std::vector<std::string> snapshot() {
        std::lock_guard<std::mutex> lk(mu);
        return { lines.begin(), lines.end() };
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mu);
        lines.clear();
    }
};
