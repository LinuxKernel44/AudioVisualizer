#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "av/core/render_settings.h"

namespace av {

struct ExportParams {
    int audioFd = -1;   // source audio (duplicated internally)
    int outFd = -1;     // seekable, read/write; the MP4 is written here (duplicated internally)
    int width = 1920;   // multiple of 4
    int height = 1080;  // even
    int fps = 30;
    int videoBitrate = 12'000'000;
    int audioBitrate = 192'000;
    int scene = -1;     // -1 = automatic (follow the structure), else a scene index
    float renderScale = 1.f;
    RenderSettings settings;
    std::string cacheDir;
};

/// Offline, deterministic video export: decode -> FrontEnd -> Vulkan offscreen render -> NV12 ->
/// H.264, audio -> AAC, both muxed into an MP4. Runs on its own thread; poll from anywhere.
class ExportJob {
public:
    enum class State : int { Running = 0, Done = 1, Failed = 2, Cancelled = 3 };

    explicit ExportJob(ExportParams p);
    ~ExportJob();
    void start();
    void cancel();

    State state() const { return state_.load(); }
    /// 0..1 (analysis counts for the first 15 %, rendering for the rest).
    float progress() const { return progress_.load(); }
    /// Frames rendered per second of wall time (rolling average).
    float renderFps() const { return renderFps_.load(); }
    std::string error() const;
    std::string phase() const;

private:
    void run();
    bool runImpl(std::string* err);
    void setPhase(const char* p);

    ExportParams p_;
    std::thread thread_;
    std::atomic<bool> cancel_{false};
    std::atomic<State> state_{State::Running};
    std::atomic<float> progress_{0.f};
    std::atomic<float> renderFps_{0.f};
    mutable std::mutex mu_;
    std::string error_;
    std::string phase_;
};

}  // namespace av
