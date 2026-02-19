#include "inference/include/inference.hpp"

#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <iostream>

namespace eeg {

    namespace {

        // epoch -> channel-first float buffer [nch, nt]
        static std::vector<float> epoch_to_ch_first(
            const std::vector<EEGSample>& epoch,
            int nch,
            int nt
        ) {
            std::vector<float> x((size_t)nch * (size_t)nt, 0.0f);

            const int n = (int)epoch.size();
            const int tmax = std::min(nt, n);

            for (int t = 0; t < tmax; ++t) {
                const auto& ch = epoch[t].channels;
                const int cmax = std::min(nch, (int)ch.size());
                for (int c = 0; c < cmax; ++c) {
                    x[(size_t)c * (size_t)nt + (size_t)t] = ch[c];
                }
            }
            return x;
        }

        static std::string to_std_string(Ort::AllocatedStringPtr& p) {
            return p ? std::string(p.get()) : std::string();
        }

    } // anonymous namespace


    struct OnnxInfer::Impl {
        Ort::Env env{ ORT_LOGGING_LEVEL_WARNING, "EEG_CORE_ONNX" };
        Ort::SessionOptions sess_opt;
        Ort::Session session{ nullptr };
        Ort::AllocatorWithDefaultOptions allocator;

        Impl(const std::wstring& model_path) {
            sess_opt.SetIntraOpNumThreads(1);
            sess_opt.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
            session = Ort::Session(env, model_path.c_str(), sess_opt);
        }
    };

    std::vector<float> OnnxInfer::softmax(const std::vector<float>& logits) {
        if (logits.empty()) return {};
        float m = *std::max_element(logits.begin(), logits.end());

        std::vector<float> expv(logits.size());
        float sum = 0.0f;
        for (size_t i = 0; i < logits.size(); ++i) {
            expv[i] = std::exp(logits[i] - m);
            sum += expv[i];
        }
        if (sum <= 0.0f) return std::vector<float>(logits.size(), 1.0f / float(logits.size()));

        for (auto& v : expv) v /= sum;
        return expv;
    }

    OnnxInfer::OnnxInfer(const std::wstring& model_path) {
        impl_ = new Impl(model_path);

        auto in0 = impl_->session.GetInputNameAllocated(0, impl_->allocator);
        auto out0 = impl_->session.GetOutputNameAllocated(0, impl_->allocator);
        input_name_ = to_std_string(in0);
        output_name_ = to_std_string(out0);

        // input shape
        {
            auto type_info = impl_->session.GetInputTypeInfo(0);
            auto tensorinfo = type_info.GetTensorTypeAndShapeInfo();
            auto shape = tensorinfo.GetShape(); // [?, ?, nch, nt]

            if (shape.size() != 4) throw std::runtime_error("Expected input rank 4");
            expected_nch_ = (shape[2] > 0) ? int(shape[2]) : -1;
            expected_nt_ = (shape[3] > 0) ? int(shape[3]) : -1;
        }

        // output shape
        {
            auto out_type = impl_->session.GetOutputTypeInfo(0);
            auto out_info = out_type.GetTensorTypeAndShapeInfo();
            auto out_shape = out_info.GetShape(); // [?, classes]

            if (out_shape.size() != 2) throw std::runtime_error("Expected output rank 2");
            n_classes_ = (out_shape[1] > 0) ? int(out_shape[1]) : -1;
            if (n_classes_ <= 0) throw std::runtime_error("Output classes dimension is dynamic/unknown");
        }

        std::cout << "[ONNX] input=" << input_name_
            << " output=" << output_name_
            << " expected_nch=" << expected_nch_
            << " expected_nt=" << expected_nt_
            << " n_classes=" << n_classes_
            << "\n";
    }

    OnnxInfer::~OnnxInfer() {
        delete impl_;
        impl_ = nullptr;
    }

    InferResult OnnxInfer::run(const float* epoch_ch_first, int nch, int nt) {
        if (!epoch_ch_first) throw std::runtime_error("epoch pointer is null");

        if (expected_nch_ > 0 && nch != expected_nch_)
            throw std::runtime_error("Input nch mismatch");
        if (expected_nt_ > 0 && nt != expected_nt_)
            throw std::runtime_error("Input nt mismatch");

        const int64_t shape[4] = { 1, 1, (int64_t)nch, (int64_t)nt };
        const size_t n_elem = (size_t)nch * (size_t)nt;

        // NOTE: this copy is OK for now
        std::vector<float> input(n_elem);
        std::copy(epoch_ch_first, epoch_ch_first + n_elem, input.begin());

        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value x = Ort::Value::CreateTensor<float>(mem, input.data(), input.size(), shape, 4);

        const char* in_names[] = { input_name_.c_str() };
        const char* out_names[] = { output_name_.c_str() };

        auto outs = impl_->session.Run(Ort::RunOptions{ nullptr },
            in_names, &x, 1,
            out_names, 1);

        float* y = outs[0].GetTensorMutableData<float>();

        InferResult r;
        r.logits.assign(y, y + n_classes_);
        r.probs = softmax(r.logits);
        r.pred = int(std::distance(r.probs.begin(),
            std::max_element(r.probs.begin(), r.probs.end())));
        return r;
    }

    InferResult OnnxInfer::run_epoch(const std::vector<EEGSample>& epoch) {
        if (epoch.empty()) throw std::runtime_error("epoch is empty");
        if (epoch.front().channels.empty()) throw std::runtime_error("epoch has no channels");

        const int nch = (expected_nch_ > 0) ? expected_nch_ : (int)epoch.front().channels.size();
        const int nt = (expected_nt_ > 0) ? expected_nt_ : (int)epoch.size();

        auto x = epoch_to_ch_first(epoch, nch, nt);
        return run(x.data(), nch, nt);
    }

    std::optional<Direction> OnnxInfer::infer_direction(const std::vector<EEGSample>& epoch) {
        InferResult r = run_epoch(epoch);

        // TODO: adjust mapping to your label order
        if (r.pred == 0) return Direction::Left;
        if (r.pred == 1) return Direction::Right;
        return std::nullopt;
    }

} // namespace eeg
