#pragma once

#include <atomic>
#include <cstdint>

namespace av {

/// Lock-free triple buffer: one producer publishes complete snapshots, one consumer always
/// sees the most recent one. Neither side ever blocks or waits for the other.
template <typename T>
class TripleBuffer {
   public:
    /// Producer: returns the slot to fill. Call publish() afterwards.
    T& writeSlot() { return slots_[writeIdx_]; }

    void publish() {
        // Swap the freshly written slot into the shared "middle" position and flag it new.
        const uint32_t prev = middle_.exchange(writeIdx_ | kNewFlag, std::memory_order_acq_rel);
        writeIdx_ = prev & kIndexMask;
    }

    /// Consumer: returns the latest published snapshot (or the previous one if none is new).
    const T& read() {
        if (middle_.load(std::memory_order_relaxed) & kNewFlag) {
            const uint32_t prev = middle_.exchange(readIdx_, std::memory_order_acq_rel);
            readIdx_ = prev & kIndexMask;
        }
        return slots_[readIdx_];
    }

   private:
    static constexpr uint32_t kNewFlag = 0x4;
    static constexpr uint32_t kIndexMask = 0x3;
    T slots_[3]{};
    uint32_t writeIdx_ = 0;
    uint32_t readIdx_ = 1;
    std::atomic<uint32_t> middle_{2};
};

}  // namespace av
