// EK.2: wear spreads evenly over the slots across 10,000 simulated saves.
//
// Tolerance: slot choice is deterministic round-robin (next = newest + 1), so after N saves
// every slot has been written either floor(N/slots) or ceil(N/slots) times: max - min <= 1.
// Anything larger means the rotation is broken. With injected power cuts, an interrupted save
// leaves the Store in place and the retry targets the same slot again, so each cut can add at
// most one extra write to one slot: max - min <= 1 + cuts.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>

#include "helpers.hpp"

using namespace eeprom;
using namespace eeprom::test;

namespace {

constexpr uint32_t kBase = 0x40;       // 64-byte aligned, like the firmware's Settings region
constexpr uint32_t kSlotSize = 1024;
constexpr uint16_t kSlots = 8;
constexpr uint32_t kSaves = 10000;

// Page write cycles of the slot's first page: every record write touches it exactly once.
size_t slot_writes(const Rig& r, uint16_t slot) {
    return r.dev.page_writes((kBase + static_cast<size_t>(slot) * kSlotSize) / r.dev.page_size());
}

void print_stats(const char* title, const Rig& r, size_t& mn, size_t& mx) {
    mn = SIZE_MAX;
    mx = 0;
    std::printf("[EK.2] %s: writes per slot (slot size %u, %u slots, page %zu B)\n", title, kSlotSize, kSlots,
                r.dev.page_size());
    for (uint16_t s = 0; s < kSlots; ++s) {
        const size_t w = slot_writes(r, s);
        mn = std::min(mn, w);
        mx = std::max(mx, w);
        std::printf("[EK.2]   slot %u: %zu\n", s, w);
    }
    std::printf("[EK.2]   min %zu, max %zu, spread %zu\n", mn, mx, mx - mn);
}

} // namespace

TEST(WearEK2, TenThousandSavesWearAllSlotsEvenly) {
    Rig r(kBase, kSlotSize, kSlots, kSlotSize, 0x2100);
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    r.dev.reset_stats();
    for (uint32_t i = 0; i < kSaves; ++i) {
        // Varying payload size (20..119 params) so records span different numbers of pages.
        ASSERT_EQ(r.save(make_items(static_cast<uint16_t>(20 + i % 100), i)), Status::Ok) << i;
    }
    size_t mn, mx;
    print_stats("clean run", r, mn, mx);

    size_t total = 0;
    for (uint16_t s = 0; s < kSlots; ++s) total += slot_writes(r, s);
    EXPECT_EQ(total, kSaves);
    EXPECT_LE(mx - mn, 1u);
    EXPECT_EQ(mn, kSaves / kSlots);

    // No page of the region is written more often than once per save of its slot.
    size_t worst_page = 0;
    for (size_t p = 0; p < r.dev.page_count(); ++p) worst_page = std::max(worst_page, r.dev.page_writes(p));
    std::printf("[EK.2]   most-written page: %zu writes (limit per slot: %u saves)\n", worst_page, (kSaves + kSlots - 1) / kSlots);
    EXPECT_LE(worst_page, (kSaves + kSlots - 1) / kSlots);
    // Nothing outside the region was touched.
    EXPECT_EQ(r.dev.page_writes(0), 0u);
    EXPECT_EQ(r.store.sequence(), kSaves);
}

TEST(WearEK2, WearStaysEvenWithRebootsAndPowerCuts) {
    Rig r(kBase, kSlotSize, kSlots, kSlotSize, 0x2100);
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    r.dev.reset_stats();
    uint32_t rng = 99;
    size_t cuts = 0;
    for (uint32_t i = 0; i < kSaves; ++i) {
        if (i % 37 == 0) { // periodic reboot: wear must not depend on in-RAM state
            r.reboot();
            ASSERT_TRUE(r.store.mount() == Status::Ok || i == 0);
        }
        const Items items = make_items(static_cast<uint16_t>(20 + i % 100), i);
        if (i % 125 == 7) { // an interrupted save, then reboot and the normal save
            rng = rng * 1664525u + 1013904223u;
            r.dev.cut_after(1 + (rng >> 8) % (16 + 8u * items.size() - 1));
            ASSERT_EQ(r.save(items), Status::IoError);
            ++cuts;
            r.reboot();
            ASSERT_TRUE(r.store.mount() == Status::Ok || r.store.mount() == Status::NoRecord);
        }
        ASSERT_EQ(r.save(items), Status::Ok) << i;
    }
    size_t mn, mx;
    print_stats("with reboots and power cuts", r, mn, mx);
    std::printf("[EK.2]   power cuts injected: %zu (tolerance: spread <= 1 + cuts = %zu)\n", cuts, 1 + cuts);
    EXPECT_GT(cuts, 70u);
    EXPECT_LE(mx - mn, 1 + cuts);
    // A cut that happens to leave a complete record (the unwritten tail already held the right
    // bytes) is a valid "new" outcome, and the next save then numbers on from it.
    EXPECT_GE(r.store.sequence(), kSaves);
    EXPECT_LE(r.store.sequence(), kSaves + cuts);
}

TEST(WearEK2, SmallRingsWearEvenlyToo) {
    for (uint16_t slots : std::initializer_list<uint16_t>{2, 3, 5, 32}) {
        Rig r(kBase, 64, slots, 64, 0x1000);
        ASSERT_EQ(r.store.mount(), Status::NoRecord);
        r.dev.reset_stats();
        for (uint32_t i = 0; i < 1000; ++i) ASSERT_EQ(r.save(make_items(6, i)), Status::Ok);
        size_t mn = SIZE_MAX, mx = 0;
        for (uint16_t s = 0; s < slots; ++s) {
            const size_t w = r.dev.page_writes((kBase + static_cast<size_t>(s) * 64) / 64);
            mn = std::min(mn, w);
            mx = std::max(mx, w);
        }
        EXPECT_LE(mx - mn, 1u) << slots << " slots";
    }
}
