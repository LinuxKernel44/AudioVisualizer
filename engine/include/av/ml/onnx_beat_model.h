#pragma once

#include <memory>
#include <string>

namespace av {

/// Beat This! ONNX model executed with ONNX Runtime (CPU). Input `spect` [1, T, 128] log-mel,
/// outputs `beat` and `downbeat` [1, T] logits (any T >= 1; the exported graph has a dynamic axis).
class OnnxBeatModel {
   public:
    OnnxBeatModel();
    ~OnnxBeatModel();
    OnnxBeatModel(const OnnxBeatModel&) = delete;
    OnnxBeatModel& operator=(const OnnxBeatModel&) = delete;

    bool load(const std::string& path, std::string* error);
    bool run(const float* spect, size_t frames, float* beatLogits, float* downbeatLogits);
    std::string lastError() const { return error_; }

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string error_;
};

}  // namespace av
