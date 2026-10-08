#pragma once

#include <atomic>
#include <cstddef>
#include <memory>

namespace av {

/// Wait-free single-producer / single-consumer ring buffer of trivially copyable items.
/// Capacity is rounded up to a power of two. Safe for use on a real-time thread.
template <typename T>
class SpscRing {
   public:
    explicit SpscRing(size_t minCapacity) {
        size_t cap = 2;
        while (cap < minCapacity) cap <<= 1;
        mask_ = cap - 1;
        data_ = std::make_unique<T[]>(cap);
    }

    size_t capacity() const { return mask_ + 1; }

    size_t readAvailable() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed);
    }
    size_t writeAvailable() const {
        return capacity() -
               (head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire));
    }

    /// Producer only. Writes up to n items, returns the number written.
    size_t write(const T* src, size_t n) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t free = capacity() - (head - tail_.load(std::memory_order_acquire));
        if (n > free) n = free;
        for (size_t i = 0; i < n; ++i) data_[(head + i) & mask_] = src[i];
        head_.store(head + n, std::memory_order_release);
        return n;
    }

    /// Consumer only. Reads up to n items, returns the number read.
    size_t read(T* dst, size_t n) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t avail = head_.load(std::memory_order_acquire) - tail;
        if (n > avail) n = avail;
        for (size_t i = 0; i < n; ++i) dst[i] = data_[(tail + i) & mask_];
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

    /// Consumer only. Drops up to n items without copying.
    size_t skip(size_t n) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t avail = head_.load(std::memory_order_acquire) - tail;
        if (n > avail) n = avail;
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

   private:
    std::unique_ptr<T[]> data_;
    size_t mask_ = 0;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

}  // namespace av
