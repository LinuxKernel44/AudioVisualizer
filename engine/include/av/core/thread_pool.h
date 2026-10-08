#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace av {

/// Fixed-size worker pool. Tasks run in FIFO order; waitIdle() blocks until the queue is empty
/// and every worker is idle.
class ThreadPool {
public:
    explicit ThreadPool(size_t threads) {
        threads = threads == 0 ? 1 : threads;
        for (size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { loop(); });
    }
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    size_t size() const { return workers_.size(); }

    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            queue_.push_back(std::move(task));
        }
        cv_.notify_one();
    }

    void waitIdle() {
        std::unique_lock<std::mutex> lk(mu_);
        idleCv_.wait(lk, [this] { return queue_.empty() && active_ == 0; });
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop_front();
                ++active_;
            }
            task();
            {
                std::lock_guard<std::mutex> lk(mu_);
                --active_;
                if (queue_.empty() && active_ == 0) idleCv_.notify_all();
            }
        }
    }

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> queue_;
    std::mutex mu_;
    std::condition_variable cv_, idleCv_;
    size_t active_ = 0;
    bool stop_ = false;
};

}  // namespace av
