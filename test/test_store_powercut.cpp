// EK.1: a power cut at every byte offset of a save leaves the old record or the new one, intact.

#include <gtest/gtest.h>

#include <cstdio>

#include "helpers.hpp"

using namespace eeprom;
using namespace eeprom::test;

namespace {

struct Config {
    const char* name;
    uint32_t base;
    uint32_t slot_size;
    uint16_t slots;
    uint16_t params;     // entries per record (u32, so a record is 16 + 8 * params bytes)
    uint32_t seed_seq;   // 0: start on blank memory; otherwise seed slot (slots-1) with this sequence
};

struct Tally {
    size_t offsets = 0, kept_old = 0, got_new = 0, scenarios = 0;
};

// Mount + load on whatever the memory now holds. Returns false if the state is "no record".
bool recover(Rig& r, Items& out, bool& had_record) {
    r.reboot();
    const Status m = r.store.mount();
    if (m == Status::NoRecord) {
        had_record = false;
        return true;
    }
    if (m != Status::Ok) return false;
    had_record = true;
    return r.load(out) == Status::Ok;
}

void run_config(const Config& cfg, Tally& t) {
    const size_t record_len = Store::kHeaderSize + 8u * cfg.params;
    const size_t cap = cfg.base + static_cast<size_t>(cfg.slot_size) * cfg.slots;
    const uint16_t saves_before_max = static_cast<uint16_t>(2 * cfg.slots + 2); // wraps the ring twice

    for (uint16_t before = 0; before <= saves_before_max; ++before) {
        // Build the "old" state: `before` completed saves (after an optional seeded record).
        Rig base(cfg.base, cfg.slot_size, cfg.slots, cfg.slot_size, cap);
        Items old_items;
        bool has_old = false;
        if (cfg.seed_seq != 0) {
            old_items = make_items(cfg.params, 1000);
            std::vector<uint8_t> payload;
            for (const auto& it : old_items) {
                const auto e = u32_entry(it.first, it.second);
                payload.insert(payload.end(), e.begin(), e.end());
            }
            write_record(base.dev, base.region, static_cast<uint16_t>(cfg.slots - 1), cfg.seed_seq, 1, payload);
            has_old = true;
        }
        EXPECT_TRUE(base.store.mount() == (has_old ? Status::Ok : Status::NoRecord));
        for (uint16_t i = 0; i < before; ++i) {
            old_items = make_items(cfg.params, 2000u + i);
            ASSERT_EQ(base.save(old_items), Status::Ok) << cfg.name;
            has_old = true;
        }
        const Items new_items = make_items(cfg.params, 9000u + before);
        const std::vector<uint8_t> snapshot = base.dev.memory();
        const uint32_t seq_before = base.store.sequence();
        ++t.scenarios;

        for (size_t cut = 0; cut <= record_len; ++cut) {
            Rig r(cfg.base, cfg.slot_size, cfg.slots, cfg.slot_size, cap);
            r.dev.memory() = snapshot;
            ASSERT_EQ(r.store.mount(), has_old ? Status::Ok : Status::NoRecord);

            r.dev.cut_after(cut);
            const Status s = r.save(new_items);
            if (cut < record_len) {
                ASSERT_EQ(s, Status::IoError) << cfg.name << " cut " << cut;
            } else {
                ASSERT_EQ(s, Status::Ok) << cfg.name << " full write";
            }
            ++t.offsets;

            Items got;
            bool had_record = false;
            ASSERT_TRUE(recover(r, got, had_record)) << cfg.name << " before=" << before << " cut=" << cut;

            if (!had_record) {
                ASSERT_FALSE(has_old) << "lost the old record: " << cfg.name << " before=" << before << " cut=" << cut;
                ++t.kept_old;
            } else if (got == new_items) {
                ++t.got_new;
                ASSERT_EQ(r.store.sequence(), seq_before + 1u);
            } else {
                ASSERT_TRUE(has_old) << "a record appeared from nowhere: " << cfg.name << " cut=" << cut;
                ASSERT_EQ(got, old_items) << "neither old nor new: " << cfg.name << " before=" << before << " cut=" << cut;
                ASSERT_EQ(r.store.sequence(), seq_before);
                ++t.kept_old;
            }
            if (cut == record_len) {
                ASSERT_TRUE(got == new_items);
            }

            // The store must keep working after the cut: save again, reboot, read it back.
            const Items next = make_items(cfg.params, 777);
            ASSERT_EQ(r.save(next), Status::Ok);
            Items again;
            ASSERT_TRUE(recover(r, again, had_record));
            ASSERT_TRUE(had_record);
            ASSERT_EQ(again, next) << cfg.name << " before=" << before << " cut=" << cut;
        }
    }
}

} // namespace

TEST(PowerCutEK1, EveryByteOffsetOfASaveLeavesOldOrNewIntact) {
    const Config configs[] = {
        {"4x128 B, 5 params", 0x40, 128, 4, 5, 0},
        {"4x128 B, 5 params, seq near wrap", 0x40, 128, 4, 5, 0xFFFFFFFDu},
        {"8x1 KB, 120 params (settings)", 0x40, 1024, 8, 120, 0},
        {"8x1 KB, 120 params, seq near wrap", 0x40, 1024, 8, 120, 0xFFFFFFFEu},
        {"32x64 B, 6 params (counters)", 0x40, 64, 32, 6, 0},
        {"2x32 B, 2 params (smallest ring)", 0x00, 32, 2, 2, 0},
    };
    size_t total_offsets = 0;
    for (const Config& cfg : configs) {
        Tally t;
        run_config(cfg, t);
        ASSERT_FALSE(HasFatalFailure());
        std::printf("[EK.1] %-36s scenarios=%3zu offsets tested=%6zu  -> old kept=%6zu  new=%6zu\n", cfg.name, t.scenarios,
                    t.offsets, t.kept_old, t.got_new);
        EXPECT_EQ(t.kept_old + t.got_new, t.offsets);
        total_offsets += t.offsets;
    }
    std::printf("[EK.1] total power-cut points tested: %zu\n", total_offsets);
}

TEST(PowerCutEK1, RandomisedCutsAndRebootsNeverLoseCommittedData) {
    Rig r(0x40, 256, 6, 256);
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    Items committed;
    bool have = false;
    uint32_t rng = 12345;
    auto next = [&]() {
        rng = rng * 1664525u + 1013904223u;
        return rng >> 8;
    };
    size_t cuts = 0, completed = 0;
    for (uint32_t i = 0; i < 4000; ++i) {
        const uint16_t params = static_cast<uint16_t>(1 + next() % 28); // up to 16 + 8*28 = 240 bytes
        const Items attempt = make_items(params, i);
        const bool cut = next() % 3 != 0;
        const size_t len = Store::kHeaderSize + 8u * params;
        if (cut) r.dev.cut_after(next() % (len + 4)); // sometimes beyond the end: no cut happens
        const Status s = r.save(attempt);
        if (s == Status::Ok) {
            ++completed;
            committed = attempt;
            have = true;
        } else {
            ASSERT_EQ(s, Status::IoError);
            ++cuts;
        }
        if (cut || next() % 5 == 0) {
            Items got;
            bool had_record = false;
            ASSERT_TRUE(recover(r, got, had_record));
            if (s == Status::Ok) {
                ASSERT_TRUE(had_record);
                ASSERT_EQ(got, attempt);
            } else if (!had_record) {
                ASSERT_FALSE(have);
            } else {
                ASSERT_TRUE(got == committed || got == attempt) << "iteration " << i;
                committed = got;
                have = true;
            }
        } else {
            r.dev.restore_power();
        }
    }
    std::printf("[EK.1] randomised: %zu saves completed, %zu interrupted by power cuts\n", completed, cuts);
    EXPECT_GT(cuts, 1000u);
    EXPECT_GT(completed, 1000u);
}
