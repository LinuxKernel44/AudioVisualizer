#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "av/core/spsc_ring.h"
#include "av/core/triple_buffer.h"

TEST(SpscRing, WriteReadWraparound) {
    av::SpscRing<int> ring(8);
    ASSERT_EQ(ring.capacity(), 8u);
    int in[6] = {1, 2, 3, 4, 5, 6}, out[6] = {};
    for (int round = 0; round < 5; ++round) {
        ASSERT_EQ(ring.write(in, 6), 6u);
        ASSERT_EQ(ring.read(out, 6), 6u);
        for (int i = 0; i < 6; ++i) ASSERT_EQ(out[i], in[i]);
    }
}

TEST(SpscRing, FullAndEmpty) {
    av::SpscRing<int> ring(4);
    int v[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    EXPECT_EQ(ring.write(v, 8), 4u);
    EXPECT_EQ(ring.writeAvailable(), 0u);
    EXPECT_EQ(ring.skip(2), 2u);
    int out[4];
    EXPECT_EQ(ring.read(out, 4), 2u);
    EXPECT_EQ(out[0], 2);
    EXPECT_EQ(ring.read(out, 4), 0u);
}

TEST(SpscRing, ConcurrentOrdering) {
    av::SpscRing<uint32_t> ring(1024);
    constexpr uint32_t kN = 2'000'000;
    std::thread producer([&] {
        for (uint32_t i = 0; i < kN;) {
            if (ring.write(&i, 1)) ++i;
        }
    });
    uint32_t expect = 0;
    while (expect < kN) {
        uint32_t v;
        if (ring.read(&v, 1)) {
            ASSERT_EQ(v, expect);
            ++expect;
        }
    }
    producer.join();
}

TEST(TripleBuffer, ConsumerSeesLatest) {
    av::TripleBuffer<int> tb;
    EXPECT_EQ(tb.read(), 0);
    tb.writeSlot() = 1;
    tb.publish();
    tb.writeSlot() = 2;
    tb.publish();
    EXPECT_EQ(tb.read(), 2);
    EXPECT_EQ(tb.read(), 2);  // no new data: stable
}

TEST(TripleBuffer, ConcurrentSnapshotsAreConsistent) {
    struct Snap {
        uint64_t a, b;
    };
    av::TripleBuffer<Snap> tb;
    std::atomic<bool> stop{false};
    std::thread producer([&] {
        for (uint64_t i = 1; !stop.load(); ++i) {
            Snap& s = tb.writeSlot();
            s.a = i;
            s.b = i;
            tb.publish();
        }
    });
    uint64_t last = 0;
    for (int i = 0; i < 500000; ++i) {
        const Snap& s = tb.read();
        ASSERT_EQ(s.a, s.b);
        ASSERT_GE(s.a, last);
        last = s.a;
    }
    stop = true;
    producer.join();
}
