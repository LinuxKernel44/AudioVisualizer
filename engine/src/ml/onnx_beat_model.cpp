#include "av/ml/onnx_beat_model.h"

#include <android/log.h>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <thread>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "av.onnx", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "av.onnx", __VA_ARGS__)

namespace av {

struct OnnxBeatModel::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "av"};
    std::unique_ptr<Ort::Session> session;
};

OnnxBeatModel::OnnxBeatModel() : impl_(std::make_unique<Impl>()) {}
OnnxBeatModel::~OnnxBeatModel() = default;

bool OnnxBeatModel::load(const std::string& path, std::string* error) {
    try {
        Ort::SessionOptions opts;
        const unsigned hc = std::max(2u, std::thread::hardware_concurrency());
        opts.SetIntraOpNumThreads(static_cast<int>(std::clamp(hc / 2, 2u, 4u)));
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        opts.AddConfigEntry("session.intra_op.allow_spinning", "0");  // do not burn cores while idle
        impl_->session = std::make_unique<Ort::Session>(impl_->env, path.c_str(), opts);
        LOGI("loaded %s (%zu inputs, %zu outputs)", path.c_str(), impl_->session->GetInputCount(), impl_->session->GetOutputCount());
        return true;
    } catch (const Ort::Exception& e) {
        error_ = e.what();
        if (error) *error = error_;
        LOGE("load failed: %s", e.what());
        return false;
    }
}

bool OnnxBeatModel::run(const float* spect, size_t frames, float* beat, float* down) {
    if (!impl_->session || frames == 0) return false;
    try {
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const int64_t shape[3] = {1, static_cast<int64_t>(frames), 128};
        Ort::Value in = Ort::Value::CreateTensor<float>(mem, const_cast<float*>(spect), frames * 128, shape, 3);
        const char* inNames[] = {"spect"};
        const char* outNames[] = {"beat", "downbeat"};
        auto out = impl_->session->Run(Ort::RunOptions{nullptr}, inNames, &in, 1, outNames, 2);
        if (out.size() != 2) return false;
        const float* b = out[0].GetTensorData<float>();
        const float* d = out[1].GetTensorData<float>();
        std::copy(b, b + frames, beat);
        std::copy(d, d + frames, down);
        return true;
    } catch (const Ort::Exception& e) {
        error_ = e.what();
        LOGE("run failed: %s", e.what());
        return false;
    }
}

}  // namespace av
