#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "av/analysis/timeline.h"

namespace av {

/// Runs the offline analysis for one file on a background thread, with an on-disk cache keyed
/// by a content fingerprint of the file. Safe to poll from any thread.
class AnalysisJob {
public:
    enum class State : int { Idle = 0, Running = 1, Done = 2, Failed = 3 };

    AnalysisJob() = default;
    ~AnalysisJob() { cancel(); }
    AnalysisJob(const AnalysisJob&) = delete;
    AnalysisJob& operator=(const AnalysisJob&) = delete;

    /// Starts analysing the audio behind `fd` (duplicated). `cacheDir` may be empty.
    void start(int fd, const std::string& cacheDir);
    void cancel() {
        cancel_.store(true);
        if (thread_.joinable()) thread_.join();
    }

    State state() const { return state_.load(); }
    float progress() const { return progress_.load(); }
    bool fromCache() const { return fromCache_.load(); }
    std::string error() const {
        std::lock_guard<std::mutex> lk(mu_);
        return error_;
    }
    /// Valid once state() == Done.
    std::shared_ptr<const Timeline> timeline() const {
        std::lock_guard<std::mutex> lk(mu_);
        return result_;
    }

private:
    void run(int fd, std::string cacheDir);

    std::thread thread_;
    std::atomic<bool> cancel_{false};
    std::atomic<State> state_{State::Idle};
    std::atomic<float> progress_{0.f};
    std::atomic<bool> fromCache_{false};
    mutable std::mutex mu_;
    std::shared_ptr<const Timeline> result_;
    std::string error_;
};

/// 64-bit content fingerprint of an open file (size plus evenly spaced sampled blocks).
uint64_t fingerprintFd(int fd);

}  // namespace av
