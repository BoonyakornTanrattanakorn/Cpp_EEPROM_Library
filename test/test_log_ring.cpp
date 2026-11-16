#include <gtest/gtest.h>

#include <array>
#include <cstdio>

#include "eeprom/log_ring.hpp"
#include "fake_device.hpp"

using namespace eeprom;
using namespace eeprom::test;

namespace {

constexpr uint32_t kBase = 0x3040; // as in the firmware's EEPROM map
constexpr uint16_t kSize = 16;

using Entry = std::array<uint8_t, kSize>;

// An entry whose bytes 4..15 identify `tag`; the first 4 bytes are the ring's sequence number.
Entry make_entry(uint32_t tag) {
    Entry e{};
    for (size_t i = 4; i < kSize; ++i) e[i] = static_cast<uint8_t>(tag * 31u + i * 7u + (tag >> 8));
    e[0] = e[1] = e[2] = e[3] = 0xEE; // must be overwritten by the ring
    return e;
}

uint32_t seq_of(const Entry& e) {
    return static_cast<uint32_t>(e[0]) | (static_cast<uint32_t>(e[1]) << 8) | (static_cast<uint32_t>(e[2]) << 16) |
           (static_cast<uint32_t>(e[3]) << 24);
}

bool same_body(const Entry& a, const Entry& b) {
    return std::equal(a.begin() + 4, a.end(), b.begin() + 4);
}

// Newest-first list of everything the ring currently returns.
std::vector<Entry> dump(LogRing& ring) {
    std::vector<Entry> out;
    for (uint16_t i = 0; i < ring.count(); ++i) {
        Entry e{};
        EXPECT_EQ(ring.read(i, e.data()), Status::Ok);
        out.push_back(e);
    }
    return out;
}

} // namespace

TEST(LogRing, BlankMountsEmpty) {
    FakeDevice dev(0x8000);
    LogRing ring(dev, kBase, kSize, 512);
    ASSERT_EQ(ring.mount(), Status::Ok);
    EXPECT_EQ(ring.count(), 0);
    EXPECT_EQ(ring.next_sequence(), 0u);
    Entry e{};
    EXPECT_EQ(ring.read(0, e.data()), Status::NotFound);
}

TEST(LogRing, UseBeforeMountIsRefused) {
    FakeDevice dev(0x8000);
    LogRing ring(dev, kBase, kSize, 16);
    const Entry e = make_entry(1);
    Entry out{};
    EXPECT_EQ(ring.append(e.data()), Status::NotMounted);
    EXPECT_EQ(ring.read(0, out.data()), Status::NotMounted);
}

TEST(LogRing, AppendStampsSequenceAndReadReturnsNewestFirst) {
    FakeDevice dev(0x8000);
    LogRing ring(dev, kBase, kSize, 16);
    ASSERT_EQ(ring.mount(), Status::Ok);
    for (uint32_t i = 0; i < 5; ++i) ASSERT_EQ(ring.append(make_entry(i).data()), Status::Ok);
    EXPECT_EQ(ring.count(), 5);
    EXPECT_EQ(ring.next_sequence(), 5u);
    for (uint16_t idx = 0; idx < 5; ++idx) {
        Entry e{};
        ASSERT_EQ(ring.read(idx, e.data()), Status::Ok);
        EXPECT_EQ(seq_of(e), 4u - idx);
        EXPECT_TRUE(same_body(e, make_entry(4u - idx)));
    }
    Entry e{};
    EXPECT_EQ(ring.read(5, e.data()), Status::NotFound);
    // On the device the entry is stored at base + 16 * index, sequence first, little-endian.
    EXPECT_EQ(dev.memory()[kBase + 16 * 2], 2);
    EXPECT_EQ(dev.memory()[kBase + 16 * 2 + 4], make_entry(2)[4]);
}

TEST(LogRing, OverwritesOldestWhenFull) {
    FakeDevice dev(0x8000);
    LogRing ring(dev, kBase, kSize, 8);
    ASSERT_EQ(ring.mount(), Status::Ok);
    for (uint32_t i = 0; i < 20; ++i) ASSERT_EQ(ring.append(make_entry(i).data()), Status::Ok);
    EXPECT_EQ(ring.count(), 8);
    const auto all = dump(ring);
    for (size_t i = 0; i < all.size(); ++i) {
        EXPECT_EQ(seq_of(all[i]), 19u - i);
        EXPECT_TRUE(same_body(all[i], make_entry(19u - static_cast<uint32_t>(i))));
    }
}

TEST(LogRing, MountAgreesWithRamStateAtEveryFillLevelAndHeadPosition) {
    for (uint16_t n : std::initializer_list<uint16_t>{1, 2, 3, 7, 8, 16}) {
        FakeDevice dev(0x8000);
        LogRing ring(dev, kBase, kSize, n);
        ASSERT_EQ(ring.mount(), Status::Ok);
        for (uint32_t i = 0; i < 4u * n + 3u; ++i) {
            ASSERT_EQ(ring.append(make_entry(i).data()), Status::Ok);
            LogRing again(dev, kBase, kSize, n);
            ASSERT_EQ(again.mount(), Status::Ok) << "n=" << n << " i=" << i;
            ASSERT_EQ(again.count(), ring.count()) << "n=" << n << " i=" << i;
            ASSERT_EQ(again.next_sequence(), ring.next_sequence()) << "n=" << n << " i=" << i;
            ASSERT_EQ(dump(again), dump(ring)) << "n=" << n << " i=" << i;
        }
    }
}

TEST(LogRing, AppendAfterMountContinuesTheSequence) {
    FakeDevice dev(0x8000);
    {
        LogRing ring(dev, kBase, kSize, 8);
        ASSERT_EQ(ring.mount(), Status::Ok);
        for (uint32_t i = 0; i < 13; ++i) ASSERT_EQ(ring.append(make_entry(i).data()), Status::Ok);
    }
    LogRing ring(dev, kBase, kSize, 8);
    ASSERT_EQ(ring.mount(), Status::Ok);
    EXPECT_EQ(ring.next_sequence(), 13u);
    ASSERT_EQ(ring.append(make_entry(13).data()), Status::Ok);
    const auto all = dump(ring);
    EXPECT_EQ(seq_of(all[0]), 13u);
    EXPECT_EQ(seq_of(all[7]), 6u);
}

TEST(LogRing, MountReadsLogarithmicallyManyEntries) {
    FakeDevice dev(0x8000);
    LogRing ring(dev, kBase, kSize, 512);
    ASSERT_EQ(ring.mount(), Status::Ok);
    for (uint32_t i = 0; i < 512u + 200u; ++i) ASSERT_EQ(ring.append(make_entry(i).data()), Status::Ok);
    LogRing again(dev, kBase, kSize, 512);
    dev.reset_stats();
    ASSERT_EQ(again.mount(), Status::Ok);
    std::printf("[LogRing] mount of a wrapped 512-entry ring: %zu small reads\n", dev.read_calls());
    EXPECT_LE(dev.read_calls(), 13u); // entry 0 + 9 probes + newest + neighbour
    EXPECT_EQ(again.count(), 512);
    EXPECT_EQ(again.next_sequence(), 712u);
}

TEST(LogRing, SequenceWrapsPastUint32WithoutUsingTheBlankMarker) {
    FakeDevice dev(0x8000);
    // Raw entries: sequence 0xFFFFFFFD, 0xFFFFFFFE, then (skipping 0xFFFFFFFF) 0, 1 in a 4-entry ring.
    const uint32_t seqs[4] = {0xFFFFFFFDu, 0xFFFFFFFEu, 0u, 1u};
    for (uint32_t i = 0; i < 4; ++i) {
        Entry e = make_entry(i);
        for (int b = 0; b < 4; ++b) e[static_cast<size_t>(b)] = static_cast<uint8_t>(seqs[i] >> (8 * b));
        ASSERT_TRUE(dev.write(kBase + i * kSize, e.data(), kSize));
    }
    LogRing ring(dev, kBase, kSize, 4);
    ASSERT_EQ(ring.mount(), Status::Ok);
    EXPECT_EQ(ring.count(), 4);
    EXPECT_EQ(ring.next_sequence(), 2u);
    Entry newest{};
    ASSERT_EQ(ring.read(0, newest.data()), Status::Ok);
    EXPECT_EQ(seq_of(newest), 1u);

    // And appending through the wrap: from 0xFFFFFFFE the next sequence is 0, not 0xFFFFFFFF.
    FakeDevice dev2(0x8000);
    Entry e0 = make_entry(0);
    e0[0] = 0xFE; e0[1] = 0xFF; e0[2] = 0xFF; e0[3] = 0xFF;
    ASSERT_TRUE(dev2.write(kBase, e0.data(), kSize));
    LogRing r2(dev2, kBase, kSize, 4);
    ASSERT_EQ(r2.mount(), Status::Ok);
    ASSERT_EQ(r2.count(), 1);
    ASSERT_EQ(r2.append(make_entry(1).data()), Status::Ok);
    ASSERT_EQ(r2.append(make_entry(2).data()), Status::Ok);
    LogRing r3(dev2, kBase, kSize, 4);
    ASSERT_EQ(r3.mount(), Status::Ok);
    EXPECT_EQ(r3.count(), 3);
    const auto all = dump(r3);
    EXPECT_EQ(seq_of(all[0]), 1u);
    EXPECT_EQ(seq_of(all[1]), 0u);
    EXPECT_EQ(seq_of(all[2]), 0xFFFFFFFEu);
}

TEST(LogRing, ClearEmptiesTheRingAndItCanBeReused) {
    FakeDevice dev(0x8000);
    LogRing ring(dev, kBase, kSize, 8);
    ASSERT_EQ(ring.mount(), Status::Ok);
    for (uint32_t i = 0; i < 11; ++i) ASSERT_EQ(ring.append(make_entry(i).data()), Status::Ok);
    ASSERT_EQ(ring.clear(), Status::Ok);
    EXPECT_EQ(ring.count(), 0);
    for (uint32_t a = kBase; a < kBase + 8 * kSize; ++a) ASSERT_EQ(dev.memory()[a], 0xFF);
    EXPECT_EQ(dev.memory()[kBase - 1], 0xFF);
    EXPECT_EQ(dev.memory()[kBase + 8 * kSize], 0xFF);

    LogRing again(dev, kBase, kSize, 8);
    ASSERT_EQ(again.mount(), Status::Ok);
    EXPECT_EQ(again.count(), 0);
    ASSERT_EQ(again.append(make_entry(100).data()), Status::Ok);
    Entry e{};
    ASSERT_EQ(again.read(0, e.data()), Status::Ok);
    EXPECT_EQ(seq_of(e), 0u);
}

TEST(LogRing, ClearOfLargeEntriesAndOddSizes) {
    for (uint16_t size : std::initializer_list<uint16_t>{4, 12, 20, 64, 100, 128}) {
        FakeDevice dev(0x4000);
        LogRing ring(dev, 0x100, size, 9);
        ASSERT_EQ(ring.mount(), Status::Ok);
        std::vector<uint8_t> e(size, 0x11);
        for (int i = 0; i < 13; ++i) ASSERT_EQ(ring.append(e.data()), Status::Ok);
        ASSERT_EQ(ring.clear(), Status::Ok);
        for (uint32_t a = 0x100; a < 0x100u + 9u * size; ++a) ASSERT_EQ(dev.memory()[a], 0xFF) << size;
        EXPECT_EQ(dev.memory()[0x100u + 9u * size], 0xFF);
    }
}

TEST(LogRing, InvalidArgumentsAreRejected) {
    FakeDevice dev(0x8000);
    EXPECT_EQ(LogRing(dev, 0, 3, 8).mount(), Status::BadArg);
    EXPECT_EQ(LogRing(dev, 0, 129, 8).mount(), Status::BadArg);
    EXPECT_EQ(LogRing(dev, 0, 16, 0).mount(), Status::BadArg);
    EXPECT_EQ(LogRing(dev, 0xFFFFFFF0u, 16, 8).mount(), Status::BadArg);
    EXPECT_EQ(LogRing(dev, 0, 16, 8).clear(), Status::Ok);
    LogRing ring(dev, 0, 16, 8);
    ASSERT_EQ(ring.mount(), Status::Ok);
    EXPECT_EQ(ring.append(nullptr), Status::BadArg);
    EXPECT_EQ(ring.read(0, nullptr), Status::BadArg);
}

TEST(LogRing, DeviceErrorsAreReportedAndDoNotAdvanceTheRing) {
    FakeDevice dev(0x8000);
    LogRing ring(dev, kBase, kSize, 8);
    ASSERT_EQ(ring.mount(), Status::Ok);
    ASSERT_EQ(ring.append(make_entry(0).data()), Status::Ok);
    dev.power_off();
    EXPECT_EQ(ring.append(make_entry(1).data()), Status::IoError);
    Entry e{};
    EXPECT_EQ(ring.read(0, e.data()), Status::IoError);
    EXPECT_EQ(ring.count(), 1);
    EXPECT_EQ(ring.next_sequence(), 1u);
    EXPECT_EQ(LogRing(dev, kBase, kSize, 8).mount(), Status::IoError);
    dev.restore_power();
    ASSERT_EQ(ring.append(make_entry(1).data()), Status::Ok);
    EXPECT_EQ(ring.count(), 2);
}

// --- power cuts ---------------------------------------------------------------------------------

TEST(LogRingPowerCut, CutAtEveryByteOfAnAppendKeepsTheRingConsistent) {
    constexpr uint16_t n = 8;
    size_t points = 0;
    // Fill levels cover: empty, partial, exactly full, and wrapped with the head at every position.
    for (uint32_t fill = 0; fill <= 3u * n; ++fill) {
        FakeDevice base(0x8000);
        {
            LogRing ring(base, kBase, kSize, n);
            ASSERT_EQ(ring.mount(), Status::Ok);
            for (uint32_t i = 0; i < fill; ++i) ASSERT_EQ(ring.append(make_entry(i).data()), Status::Ok);
        }
        LogRing before_ring(base, kBase, kSize, n);
        ASSERT_EQ(before_ring.mount(), Status::Ok);
        const std::vector<Entry> old_list = dump(before_ring);
        const uint32_t old_next = before_ring.next_sequence();
        const Entry fresh = make_entry(1000 + fill);

        for (size_t cut = 0; cut <= kSize; ++cut) {
            FakeDevice dev(0x8000);
            dev.memory() = base.memory();
            LogRing ring(dev, kBase, kSize, n);
            ASSERT_EQ(ring.mount(), Status::Ok);
            dev.cut_after(cut);
            const Status s = ring.append(fresh.data());
            ASSERT_EQ(s, cut < kSize ? Status::IoError : Status::Ok);
            ++points;

            dev.restore_power();
            LogRing after(dev, kBase, kSize, n);
            ASSERT_EQ(after.mount(), Status::Ok) << "fill=" << fill << " cut=" << cut;
            ASSERT_LE(after.count(), n);
            const std::vector<Entry> list = dump(after);

            if (cut == 0) {
                ASSERT_EQ(list, old_list) << "fill=" << fill;
            } else if (cut >= 4) {
                // The sequence number landed, so the new entry is the newest; its tail may be torn.
                ASSERT_EQ(after.next_sequence(), old_next + 1u) << "fill=" << fill << " cut=" << cut;
                ASSERT_EQ(seq_of(list[0]), old_next);
                for (size_t b = 4; b < cut; ++b) ASSERT_EQ(list[0][b], fresh[b]) << "byte " << b;
                // Everything older is intact (the very oldest is gone only if the ring was full).
                const size_t keep = std::min<size_t>(old_list.size(), n - 1u);
                ASSERT_EQ(list.size(), std::min<size_t>(old_list.size() + 1, n));
                for (size_t i = 0; i < keep; ++i) ASSERT_EQ(list[i + 1], old_list[i]) << "fill=" << fill << " cut=" << cut;
                if (cut == kSize) {
                    ASSERT_TRUE(same_body(list[0], fresh));
                }
            } else {
                // Cut inside the 4-byte sequence number: impossible within one real page write.
                // The ring must still mount and be readable; at most the oldest entry is lost.
                ASSERT_GE(list.size() + 1, old_list.size());
            }
        }
    }
    std::printf("[LogRing] power-cut points tested: %zu\n", points);
}

TEST(LogRingPowerCut, CutDuringClearAtBlockBoundariesLeavesAConsistentRing) {
    constexpr uint16_t n = 24; // 24 x 16 B = 384 B: 6 erase blocks of 64 B
    for (uint32_t fill : {5u, 24u, 30u, 47u}) { // partial, full, and two wrapped positions
        FakeDevice base(0x8000);
        {
            LogRing ring(base, kBase, kSize, n);
            ASSERT_EQ(ring.mount(), Status::Ok);
            for (uint32_t i = 0; i < fill; ++i) ASSERT_EQ(ring.append(make_entry(i).data()), Status::Ok);
        }
        LogRing orig(base, kBase, kSize, n);
        ASSERT_EQ(orig.mount(), Status::Ok);
        const std::vector<Entry> old_list = dump(orig);

        for (size_t cut = 0; cut <= n * kSize; cut += 64) {
            FakeDevice dev(0x8000);
            dev.memory() = base.memory();
            LogRing ring(dev, kBase, kSize, n);
            ASSERT_EQ(ring.mount(), Status::Ok);
            dev.cut_after(cut);
            const Status s = ring.clear();
            ASSERT_EQ(s, cut < n * kSize ? Status::IoError : Status::Ok);

            dev.restore_power();
            LogRing after(dev, kBase, kSize, n);
            ASSERT_EQ(after.mount(), Status::Ok) << "fill=" << fill << " cut=" << cut;
            const std::vector<Entry> list = dump(after);
            ASSERT_LE(list.size(), old_list.size());
            // What survives is a run of consecutive old entries, newest first, with the right contents.
            for (size_t i = 0; i < list.size(); ++i) {
                if (i > 0) {
                    ASSERT_EQ(seq_of(list[i]), seq_of(list[i - 1]) - 1u);
                }
                const uint32_t seq = seq_of(list[i]);
                const Entry expect = make_entry(seq);
                ASSERT_TRUE(same_body(list[i], expect)) << "fill=" << fill << " cut=" << cut;
            }
            if (cut >= n * kSize) {
                ASSERT_TRUE(list.empty());
            }
        }
    }
}

TEST(LogRingPowerCut, CutAtEveryByteOfAClearStillMounts) {
    constexpr uint16_t n = 10;
    FakeDevice base(0x8000);
    {
        LogRing ring(base, kBase, kSize, n);
        ASSERT_EQ(ring.mount(), Status::Ok);
        for (uint32_t i = 0; i < 14; ++i) ASSERT_EQ(ring.append(make_entry(i).data()), Status::Ok);
    }
    for (size_t cut = 0; cut < n * kSize; ++cut) {
        FakeDevice dev(0x8000);
        dev.memory() = base.memory();
        LogRing ring(dev, kBase, kSize, n);
        ASSERT_EQ(ring.mount(), Status::Ok);
        dev.cut_after(cut);
        ASSERT_EQ(ring.clear(), Status::IoError);
        dev.restore_power();
        LogRing after(dev, kBase, kSize, n);
        ASSERT_EQ(after.mount(), Status::Ok) << cut;
        ASSERT_LE(after.count(), n);
        for (uint16_t i = 0; i < after.count(); ++i) {
            Entry e{};
            ASSERT_EQ(after.read(i, e.data()), Status::Ok);
        }
        // Whatever is left, the ring accepts new entries and returns them newest first.
        ASSERT_EQ(after.append(make_entry(500).data()), Status::Ok);
        Entry e{};
        ASSERT_EQ(after.read(0, e.data()), Status::Ok);
        ASSERT_TRUE(same_body(e, make_entry(500)));
    }
}
