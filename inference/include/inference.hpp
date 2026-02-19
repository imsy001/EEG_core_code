#pragma once

#include <string>
#include <vector>
#include <optional>

// You must include the definitions of EEGSample + Direction.
// Pick the correct header in your project.
// I'm guessing controller.hpp (or a shared types header) contains them.
#include "core/include/controller.hpp" 
// If that's too heavy, create a small "types.hpp" that defines EEGSample/Direction and include that instead.

namespace eeg {

    struct InferResult {
        std::vector<float> logits;
        std::vector<float> probs;
        int pred = -1;
    };

    class OnnxInfer {
    public:
        explicit OnnxInfer(const std::wstring& model_path);
        ~OnnxInfer();

        // Low-level: expects channel-first [nch, nt] contiguous float buffer
        InferResult run(const float* epoch_ch_first, int nch, int nt);

        // High-level: takes Controller epoch directly (in-memory)
        InferResult run_epoch(const std::vector<EEGSample>& epoch);

        // Convenience: map prediction -> Direction (adjust mapping to your label order)
        std::optional<Direction> infer_direction(const std::vector<EEGSample>& epoch);

        // Optional: expose model expected dims so Controller can normalize correctly
        int expected_nch() const { return expected_nch_; }
        int expected_nt()  const { return expected_nt_; }
        int n_classes()    const { return n_classes_; }

    private:
        struct Impl;
        Impl* impl_ = nullptr;

        std::string input_name_;
        std::string output_name_;

        int expected_nch_ = -1;
        int expected_nt_ = -1;
        int n_classes_ = -1;

        static std::vector<float> softmax(const std::vector<float>& logits);
    };

} // namespace eeg
