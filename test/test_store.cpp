#include <gtest/gtest.h>

#include <cmath>

#include "helpers.hpp"

using namespace eeprom;
using namespace eeprom::test;

namespace {

// Small default rig: 4 slots of 128 bytes at 0x40.
Rig small_rig() { return Rig(0x40, 128, 4, 128); }

} // namespace

TEST(Store, BlankEepromMountsAsNoRecord) {
    Rig r = small_rig();
    EXPECT_EQ(r.store.mount(), Status::NoRecord);
    EXPECT_FALSE(r.store.has_record());
    Items out;
    EXPECT_EQ(r.load(out), Status::NoRecord);
    EXPECT_EQ(r.store.next_slot(), 0);
}

TEST(Store, SaveBeforeMountIsRefused) {
    Rig r = small_rig();
    EXPECT_EQ(r.save(make_items(2, 1)), Status::NotMounted);
    Items out;
    EXPECT_EQ(r.load(out), Status::NotMounted);
    EXPECT_EQ(r.dev.bytes_written(), 0u);
}

TEST(Store, FirstSaveOnBlankGoesToSlot0WithSequence1) {
    Rig r = small_rig();
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    ASSERT_EQ(r.save(make_items(3, 1)), Status::Ok);
    EXPECT_TRUE(r.store.has_record());
    EXPECT_EQ(r.store.active_slot(), 0);
    EXPECT_EQ(r.store.sequence(), 1u);
}

TEST(Store, RecordFormatMatchesTheDocumentedLayout) {
    Rig r = small_rig();
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    const Items items = {{7, 0x11223344u}};
    ASSERT_EQ(r.save(items, 3), Status::Ok);

    const uint8_t expect[] = {
        0x43, 0x4C, 0x51, 0x4F, // magic
        0x01, 0x00, 0x00, 0x00, // sequence 1
        0x03, 0x00,             // layout 3
        0x08, 0x00,             // length 8
        0xA7, 0x57, 0x3F, 0x70, // crc32 over sequence..payload (crc field read as zero), 0x703F57A7
        0x07, 0x00, 0x00, 0x04, 0x44, 0x33, 0x22, 0x11, // entry: id 7, U32, len 4, value
    };
    const auto& m = r.dev.memory();
    EXPECT_EQ(std::vector<uint8_t>(m.begin() + 0x40, m.begin() + 0x40 + sizeof(expect)),
              std::vector<uint8_t>(expect, expect + sizeof(expect)));
    EXPECT_EQ(m[0x40 + sizeof(expect)], 0xFF) << "bytes after the record are not written";
    EXPECT_EQ(r.dev.bytes_written(), sizeof(expect));
    EXPECT_EQ(r.dev.write_calls(), 1u) << "one device write per save";
}

TEST(Store, RoundTripsEveryType) {
    Rig r = small_rig();
    ASSERT_EQ(r.store.mount(), Status::NoRecord);

    struct Seen {
        uint32_t u = 0;
        int32_t i = 0;
        float f = 0;
        bool b = false;
        uint8_t blob[5] = {};
        uint8_t blob_len = 0;
    } seen;
    auto emit = [](void*, Writer& w) {
        const uint8_t blob[5] = {1, 2, 3, 4, 5};
        Status s = w.put_u32(1, 0xDEADBEEFu);
        if (s == Status::Ok) s = w.put_i32(2, -123456);
        if (s == Status::Ok) s = w.put_f32(3, 3.25f);
        if (s == Status::Ok) s = w.put_bool(4, true);
        if (s == Status::Ok) s = w.put_blob(5, blob, 5);
        return s;
    };
    ASSERT_EQ(r.store.save(emit, nullptr, 9), Status::Ok);

    r.reboot();
    ASSERT_EQ(r.store.mount(), Status::Ok);
    auto on = [](void* c, uint16_t id, Type t, const uint8_t* v, uint8_t len) {
        auto& s = *static_cast<Seen*>(c);
        switch (id) {
        case 1: EXPECT_EQ(t, Type::U32); s.u = get_u32(v); break;
        case 2: EXPECT_EQ(t, Type::I32); s.i = get_i32(v); break;
        case 3: EXPECT_EQ(t, Type::F32); s.f = get_f32(v); break;
        case 4: EXPECT_EQ(t, Type::BOOL); s.b = v[0] != 0; break;
        case 5: EXPECT_EQ(t, Type::BLOB); s.blob_len = len; std::memcpy(s.blob, v, len); break;
        }
        return EntryResult::Applied;
    };
    uint16_t layout = 0;
    LoadReport rep;
    ASSERT_EQ(r.store.load(on, &seen, &layout, &rep), Status::Ok);
    EXPECT_EQ(seen.u, 0xDEADBEEFu);
    EXPECT_EQ(seen.i, -123456);
    EXPECT_EQ(seen.f, 3.25f);
    EXPECT_TRUE(seen.b);
    EXPECT_EQ(seen.blob_len, 5);
    EXPECT_EQ(seen.blob[4], 5);
    EXPECT_EQ(layout, 9);
    EXPECT_EQ(rep.entries, 5);
    EXPECT_EQ(rep.applied, 5);
    EXPECT_EQ(rep.unknown + rep.out_of_range + rep.malformed, 0);
}

TEST(Store, EmptyPayloadIsAValidRecord) {
    Rig r = small_rig();
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    ASSERT_EQ(r.save(Items{}), Status::Ok);
    r.reboot();
    ASSERT_EQ(r.store.mount(), Status::Ok);
    Items out;
    LoadReport rep;
    EXPECT_EQ(r.load(out, nullptr, &rep), Status::Ok);
    EXPECT_EQ(rep.entries, 0);
}

TEST(Store, SavesRotateThroughSlotsInOrder) {
    Rig r = small_rig();
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    for (uint32_t i = 1; i <= 10; ++i) {
        ASSERT_EQ(r.save(make_items(2, i)), Status::Ok);
        EXPECT_EQ(r.store.sequence(), i);
        EXPECT_EQ(r.store.active_slot(), (i - 1) % 4);
        EXPECT_EQ(r.store.next_slot(), i % 4);
    }
}

TEST(Store, MountAfterRebootFindsTheNewestRecord) {
    Rig r = small_rig();
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    for (uint32_t i = 1; i <= 6; ++i) ASSERT_EQ(r.save(make_items(4, i)), Status::Ok);
    r.reboot();
    ASSERT_EQ(r.store.mount(), Status::Ok);
    EXPECT_EQ(r.store.sequence(), 6u);
    EXPECT_EQ(r.store.active_slot(), 1);
    Items out;
    ASSERT_EQ(r.load(out), Status::Ok);
    EXPECT_EQ(out, make_items(4, 6));
    ASSERT_EQ(r.save(make_items(4, 7)), Status::Ok);
    EXPECT_EQ(r.store.active_slot(), 2);
    EXPECT_EQ(r.store.sequence(), 7u);
}

TEST(Store, SaveNeverWritesTheNewestSlot) {
    Rig r = small_rig();
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    for (uint32_t i = 1; i <= 12; ++i) {
        const uint16_t newest = r.store.has_record() ? r.store.active_slot() : 0xFFFF;
        std::vector<uint8_t> before = r.dev.memory();
        ASSERT_EQ(r.save(make_items(5, i)), Status::Ok);
        if (newest != 0xFFFF) {
            const size_t a = 0x40 + static_cast<size_t>(newest) * 128;
            EXPECT_TRUE(std::equal(before.begin() + static_cast<long>(a), before.begin() + static_cast<long>(a) + 128,
                                   r.dev.memory().begin() + static_cast<long>(a)));
        }
    }
}

// --- load semantics ---------------------------------------------------------------------

TEST(StoreLoad, UnknownIdsAreIgnoredAndMissingIdsKeepDefaults) {
    // Saved by "old firmware": ids 1, 2, 99 (99 was later removed). New firmware knows 1, 2, 3 (new).
    Rig r = small_rig();
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    ASSERT_EQ(r.save(Items{{1, 11}, {2, 22}, {99, 9999}}), Status::Ok);
    r.reboot();
    ASSERT_EQ(r.store.mount(), Status::Ok);

    struct App {
        uint32_t p1 = 100, p2 = 200, p3 = 300; // defaults
    } app;
    auto on = [](void* c, uint16_t id, Type, const uint8_t* v, uint8_t) {
        auto& a = *static_cast<App*>(c);
        switch (id) {
        case 1: a.p1 = get_u32(v); return EntryResult::Applied;
        case 2: a.p2 = get_u32(v); return EntryResult::Applied;
        case 3: a.p3 = get_u32(v); return EntryResult::Applied;
        default: return EntryResult::Unknown;
        }
    };
    LoadReport rep;
    ASSERT_EQ(r.store.load(on, &app, nullptr, &rep), Status::Ok);
    EXPECT_EQ(app.p1, 11u);
    EXPECT_EQ(app.p2, 22u);
    EXPECT_EQ(app.p3, 300u) << "id 3 is absent from the record: default kept";
    EXPECT_EQ(rep.entries, 3);
    EXPECT_EQ(rep.applied, 2);
    EXPECT_EQ(rep.unknown, 1);
}

TEST(StoreLoad, OutOfRangeIsReportedAndDefaultKept) {
    Rig r = small_rig();
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    ASSERT_EQ(r.save(Items{{1, 50}, {2, 5000}, {3, 7}}), Status::Ok);
    r.reboot();
    ASSERT_EQ(r.store.mount(), Status::Ok);

    struct App {
        uint32_t p[4] = {0, 10, 10, 10}; // defaults; valid range 0..100
    } app;
    auto on = [](void* c, uint16_t id, Type, const uint8_t* v, uint8_t) {
        auto& a = *static_cast<App*>(c);
        const uint32_t x = get_u32(v);
        if (x > 100) return EntryResult::OutOfRange;
        a.p[id] = x;
        return EntryResult::Applied;
    };
    LoadReport rep;
    ASSERT_EQ(r.store.load(on, &app, nullptr, &rep), Status::Ok);
    EXPECT_EQ(app.p[1], 50u);
    EXPECT_EQ(app.p[2], 10u) << "default kept";
    EXPECT_EQ(app.p[3], 7u);
    EXPECT_EQ(rep.out_of_range, 1);
    EXPECT_EQ(rep.applied, 2);
}

TEST(StoreLoad, UnknownTypeAndWrongLengthAreSkippedWithoutCallback) {
    Rig r = small_rig();
    std::vector<uint8_t> payload = u32_entry(1, 5);
    const uint8_t unknown_type[] = {2, 0, 0x7F, 2, 0xAA, 0xBB};  // type 0x7F from a newer library
    const uint8_t bad_len[] = {3, 0, 0 /*U32*/, 2, 0x01, 0x02}; // U32 with len 2
    payload.insert(payload.end(), unknown_type, unknown_type + sizeof(unknown_type));
    payload.insert(payload.end(), bad_len, bad_len + sizeof(bad_len));
    const std::vector<uint8_t> tail = u32_entry(4, 6);
    payload.insert(payload.end(), tail.begin(), tail.end());
    write_record(r.dev, r.region, 2, 40, 1, payload);

    ASSERT_EQ(r.store.mount(), Status::Ok);
    Items out;
    LoadReport rep;
    ASSERT_EQ(r.load(out, nullptr, &rep), Status::Ok);
    EXPECT_EQ(out, (Items{{1, 5}, {4, 6}}));
    EXPECT_EQ(rep.entries, 4);
    EXPECT_EQ(rep.unknown, 1);
    EXPECT_EQ(rep.malformed, 1);
    EXPECT_EQ(rep.applied, 2);
}

TEST(StoreLoad, StructurallyBrokenPayloadWithValidCrcIsCorrupt) {
    Rig r = small_rig();
    std::vector<uint8_t> payload = u32_entry(1, 5);
    payload.push_back(0x09); // 1 stray byte: not a whole entry header
    write_record(r.dev, r.region, 0, 1, 1, payload);
    ASSERT_EQ(r.store.mount(), Status::Ok);
    Items out;
    EXPECT_EQ(r.load(out), Status::Corrupt);

    std::vector<uint8_t> overrun = {1, 0, 4, 200, 1, 2}; // claims 200 value bytes
    write_record(r.dev, r.region, 1, 2, 1, overrun);
    ASSERT_EQ(r.store.mount(), Status::Ok);
    EXPECT_EQ(r.load(out), Status::Corrupt);
}

TEST(StoreLoad, LayoutVersionIsReturned) {
    Rig r = small_rig();
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    ASSERT_EQ(r.save(make_items(1, 1), 0xBEEF), Status::Ok);
    Items out;
    uint16_t layout = 0;
    ASSERT_EQ(r.load(out, &layout), Status::Ok);
    EXPECT_EQ(layout, 0xBEEF);
}

TEST(StoreLoad, RecordThatRotsAfterMountIsReportedCorrupt) {
    Rig r = small_rig();
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    ASSERT_EQ(r.save(make_items(3, 1)), Status::Ok);
    r.dev.flip_bit(0x40 + 20, 2);
    Items out;
    EXPECT_EQ(r.load(out), Status::Corrupt);
}

// --- sizes -------------------------------------------------------------------------------

TEST(Store, PayloadTooLargeIsRefusedAndChangesNothing) {
    Rig r = small_rig(); // 128-byte slots: 112 bytes of payload = 14 u32 entries
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    ASSERT_EQ(r.save(make_items(14, 1)), Status::Ok) << "exact fit";
    const auto mem = r.dev.memory();
    EXPECT_EQ(r.save(make_items(15, 2)), Status::TooLarge);
    EXPECT_EQ(r.dev.memory(), mem);
    EXPECT_EQ(r.store.sequence(), 1u);
}

TEST(Store, ScratchSmallerThanSlotLimitsThePayload) {
    Rig r(0x40, 256, 4, 48); // 48-byte scratch: 32 bytes of payload = 4 entries
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    EXPECT_EQ(r.save(make_items(4, 1)), Status::Ok);
    EXPECT_EQ(r.save(make_items(5, 1)), Status::TooLarge);
}

TEST(Store, RecordLargerThanScratchIsAnErrorNotAnEmptyStore) {
    Rig big(0x40, 256, 4, 256);
    ASSERT_EQ(big.store.mount(), Status::NoRecord);
    ASSERT_EQ(big.save(make_items(20, 1)), Status::Ok); // 176-byte record
    Rig small(0x40, 256, 4, 64);
    small.dev.memory() = big.dev.memory();
    EXPECT_EQ(small.store.mount(), Status::TooLarge);
}

TEST(Store, InvalidRegionsAreRejectedByMount) {
    uint8_t scratch[64];
    FakeDevice dev(0x1000);
    auto mount = [&](Region reg, size_t slen = sizeof(scratch)) { return Store(dev, reg, scratch, slen).mount(); };
    EXPECT_EQ(mount({0, 128, 1, kMagic}), Status::BadArg) << "needs at least 2 slots";
    EXPECT_EQ(mount({0, 16, 4, kMagic}), Status::BadArg) << "slot must hold more than the header";
    EXPECT_EQ(mount({0xFFFFFF00u, 128, 4, kMagic}), Status::BadArg) << "address overflow";
    EXPECT_EQ(mount({0, 128, 4, kMagic}, 16), Status::BadArg) << "scratch must exceed the header";
    EXPECT_EQ(mount({0, 128, 4, kMagic}), Status::NoRecord);
}

// --- corruption and foreign data ------------------------------------------------------------

TEST(StoreCorruption, CorruptNewestFallsBackToThePreviousRecord) {
    Rig r = small_rig();
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    for (uint32_t i = 1; i <= 3; ++i) ASSERT_EQ(r.save(make_items(4, i)), Status::Ok);
    r.dev.flip_bit(0x40 + 2 * 128 + 24, 5); // payload of seq 3 (slot 2)
    r.reboot();
    ASSERT_EQ(r.store.mount(), Status::Ok);
    EXPECT_EQ(r.store.sequence(), 2u);
    EXPECT_EQ(r.store.active_slot(), 1);
    Items out;
    ASSERT_EQ(r.load(out), Status::Ok);
    EXPECT_EQ(out, make_items(4, 2));
    // The next save overwrites the corrupt slot and carries on from the good record.
    ASSERT_EQ(r.save(make_items(4, 9)), Status::Ok);
    EXPECT_EQ(r.store.active_slot(), 2);
    EXPECT_EQ(r.store.sequence(), 3u);
}

TEST(StoreCorruption, EverySingleBitFlipInTheNewestRecordIsDetected) {
    Rig base = small_rig();
    ASSERT_EQ(base.store.mount(), Status::NoRecord);
    ASSERT_EQ(base.save(make_items(3, 1)), Status::Ok);
    ASSERT_EQ(base.save(make_items(3, 2)), Status::Ok); // slot 1, 16 + 24 = 40 bytes
    const size_t rec = 0x40 + 128, len = 40;
    for (size_t byte = 0; byte < len; ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            Rig r = small_rig();
            r.dev.memory() = base.dev.memory();
            r.dev.flip_bit(static_cast<uint32_t>(rec + byte), bit);
            ASSERT_EQ(r.store.mount(), Status::Ok);
            ASSERT_EQ(r.store.sequence(), 1u) << "byte " << byte << " bit " << bit << " went undetected";
            Items out;
            ASSERT_EQ(r.load(out), Status::Ok);
            ASSERT_EQ(out, make_items(3, 1));
        }
    }
}

TEST(StoreCorruption, AllSlotsCorruptMeansNoRecordAndSaveStillWorks) {
    Rig r = small_rig();
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    for (uint32_t i = 1; i <= 4; ++i) ASSERT_EQ(r.save(make_items(2, i)), Status::Ok);
    for (uint32_t s = 0; s < 4; ++s) r.dev.flip_bit(0x40 + s * 128 + 18, 0);
    r.reboot();
    EXPECT_EQ(r.store.mount(), Status::NoRecord);
    ASSERT_EQ(r.save(make_items(2, 77)), Status::Ok);
    EXPECT_EQ(r.store.active_slot(), 0);
    EXPECT_EQ(r.store.sequence(), 1u);
    r.reboot();
    ASSERT_EQ(r.store.mount(), Status::Ok);
    Items out;
    ASSERT_EQ(r.load(out), Status::Ok);
    EXPECT_EQ(out, make_items(2, 77));
}

TEST(StoreCorruption, ImplausibleLengthInTheHeaderIsRejectedWithoutReadingFar) {
    Rig r = small_rig();
    std::vector<uint8_t> hdr(16, 0);
    hdr[0] = 0x43; hdr[1] = 0x4C; hdr[2] = 0x51; hdr[3] = 0x4F; // right magic
    hdr[4] = 5;
    hdr[10] = 0xFF; hdr[11] = 0xFF;                              // length 65535
    ASSERT_TRUE(r.dev.write(0x40, hdr.data(), hdr.size()));
    EXPECT_EQ(r.store.mount(), Status::NoRecord);
}

TEST(StoreCorruption, ForeignRegionRecordsAreIgnored) {
    Rig r = small_rig();
    write_record(r.dev, r.region, 1, 50, 1, u32_entry(1, 1), 0x12345678u);
    EXPECT_EQ(r.store.mount(), Status::NoRecord);
}

// --- sequence numbers ---------------------------------------------------------------------

TEST(StoreSequence, NewestWinsAcrossTheUint32Wrap) {
    Rig r = small_rig();
    write_record(r.dev, r.region, 0, 0xFFFFFFFEu, 1, u32_entry(1, 100));
    write_record(r.dev, r.region, 1, 0xFFFFFFFFu, 1, u32_entry(1, 101));
    write_record(r.dev, r.region, 2, 0x00000000u, 1, u32_entry(1, 102));
    write_record(r.dev, r.region, 3, 0x00000001u, 1, u32_entry(1, 103));
    ASSERT_EQ(r.store.mount(), Status::Ok);
    EXPECT_EQ(r.store.sequence(), 1u);
    EXPECT_EQ(r.store.active_slot(), 3);
    Items out;
    ASSERT_EQ(r.load(out), Status::Ok);
    EXPECT_EQ(out, (Items{{1, 103}}));
}

TEST(StoreSequence, WrapIsHandledWhateverTheSlotOrder) {
    // Rotate which slot holds which sequence number: the newest must win for all alignments.
    for (uint16_t start = 0; start < 4; ++start) {
        Rig r = small_rig();
        const uint32_t seqs[4] = {0xFFFFFFFDu, 0xFFFFFFFEu, 0xFFFFFFFFu, 0u};
        for (uint16_t i = 0; i < 4; ++i)
            write_record(r.dev, r.region, static_cast<uint16_t>((start + i) % 4), seqs[i], 1, u32_entry(1, i));
        ASSERT_EQ(r.store.mount(), Status::Ok);
        EXPECT_EQ(r.store.sequence(), 0u) << start;
        EXPECT_EQ(r.store.active_slot(), (start + 3) % 4) << start;
    }
}

TEST(StoreSequence, SavesContinueThroughTheWrap) {
    Rig r = small_rig();
    write_record(r.dev, r.region, 0, 0xFFFFFFFDu, 1, u32_entry(1, 0));
    ASSERT_EQ(r.store.mount(), Status::Ok);
    const uint32_t expect_seq[] = {0xFFFFFFFEu, 0xFFFFFFFFu, 0u, 1u, 2u, 3u};
    for (uint32_t i = 0; i < 6; ++i) {
        ASSERT_EQ(r.save(make_items(2, i + 10)), Status::Ok);
        EXPECT_EQ(r.store.sequence(), expect_seq[i]);
        r.reboot();
        ASSERT_EQ(r.store.mount(), Status::Ok);
        EXPECT_EQ(r.store.sequence(), expect_seq[i]) << "mount must agree after save " << i;
        Items out;
        ASSERT_EQ(r.load(out), Status::Ok);
        EXPECT_EQ(out, make_items(2, i + 10));
    }
}

TEST(StoreSequence, CorruptNewestAtTheWrapFallsBackToTheOlderSequence) {
    Rig r = small_rig();
    write_record(r.dev, r.region, 0, 0xFFFFFFFFu, 1, u32_entry(1, 1));
    write_record(r.dev, r.region, 1, 0u, 1, u32_entry(1, 2));
    r.dev.flip_bit(0x40 + 128 + 17, 1);
    ASSERT_EQ(r.store.mount(), Status::Ok);
    EXPECT_EQ(r.store.sequence(), 0xFFFFFFFFu);
}

TEST(StoreSequence, EqualSequencesDoNotConfuseMount) {
    Rig r = small_rig();
    write_record(r.dev, r.region, 0, 7, 1, u32_entry(1, 1));
    write_record(r.dev, r.region, 2, 7, 1, u32_entry(1, 2));
    ASSERT_EQ(r.store.mount(), Status::Ok);
    EXPECT_EQ(r.store.sequence(), 7u);
    EXPECT_EQ(r.store.active_slot(), 2) << "deterministic: the higher slot wins a tie";
}

// --- pluggable CRC, verify, I/O errors -----------------------------------------------------------

namespace {
uint32_t xor_crc(const uint8_t* d, size_t n) { return soft_crc32(d, n) ^ 0xA5A5A5A5u; }
} // namespace

TEST(StoreCrc, PluggableCrcIsUsedForSaveAndMount) {
    Rig r = small_rig();
    Store custom(r.dev, r.region, r.scratch.data(), r.scratch.size(), &xor_crc);
    ASSERT_EQ(custom.mount(), Status::NoRecord);
    const Items items = make_items(2, 3);
    ASSERT_EQ(custom.save(&emit_items, const_cast<Items*>(&items), 1), Status::Ok);

    Store same(r.dev, r.region, r.scratch.data(), r.scratch.size(), &xor_crc);
    EXPECT_EQ(same.mount(), Status::Ok);
    Store other(r.dev, r.region, r.scratch.data(), r.scratch.size()); // default CRC
    EXPECT_EQ(other.mount(), Status::NoRecord) << "records under another CRC do not validate";
}

TEST(StoreVerify, ReadBackCatchesAWriteThatStoredTheWrongBit) {
    Rig r = small_rig();
    r.store.set_verify_after_write(true);
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    ASSERT_EQ(r.save(make_items(3, 1)), Status::Ok);
    r.dev.flip_on_write(20, 0x01);
    EXPECT_EQ(r.save(make_items(3, 2)), Status::VerifyFailed);
    EXPECT_EQ(r.store.sequence(), 1u) << "failed save does not advance the store";
    // The bad slot fails its CRC, so a reboot still yields the first record.
    r.reboot();
    ASSERT_EQ(r.store.mount(), Status::Ok);
    EXPECT_EQ(r.store.sequence(), 1u);
}

TEST(StoreVerify, CleanWritesPassVerification) {
    Rig r = small_rig();
    r.store.set_verify_after_write(true);
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    for (uint32_t i = 0; i < 9; ++i) EXPECT_EQ(r.save(make_items(14, i)), Status::Ok);
}

TEST(StoreIo, DeviceFailuresAreReported) {
    Rig r = small_rig();
    r.dev.power_off();
    EXPECT_EQ(r.store.mount(), Status::IoError);
    r.dev.restore_power();
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    ASSERT_EQ(r.save(make_items(2, 1)), Status::Ok);
    r.dev.power_off();
    EXPECT_EQ(r.save(make_items(2, 2)), Status::IoError);
    Items out;
    EXPECT_EQ(r.load(out), Status::IoError);
    EXPECT_EQ(r.store.sequence(), 1u);
}

TEST(StoreIo, FailedSaveRetriesTheSameSlot) {
    Rig r = small_rig();
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    ASSERT_EQ(r.save(make_items(2, 1)), Status::Ok);
    r.dev.cut_after(10);
    EXPECT_EQ(r.save(make_items(2, 2)), Status::IoError);
    r.dev.restore_power();
    EXPECT_EQ(r.store.next_slot(), 1);
    ASSERT_EQ(r.save(make_items(2, 3)), Status::Ok);
    EXPECT_EQ(r.store.active_slot(), 1);
    EXPECT_EQ(r.store.sequence(), 2u);
}

TEST(StoreIo, EmitFailurePropagatesAndWritesNothing) {
    Rig r = small_rig();
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    auto bad = [](void*, Writer&) { return Status::BadArg; };
    EXPECT_EQ(r.store.save(bad, nullptr, 1), Status::BadArg);
    auto ignores_error = [](void*, Writer& w) {
        uint8_t big[200] = {};
        w.put_blob(1, big, 200); // 204 > 112: overflow, but the callback pretends all is well
        return Status::Ok;
    };
    EXPECT_EQ(r.store.save(ignores_error, nullptr, 1), Status::TooLarge);
    EXPECT_EQ(r.dev.bytes_written(), 0u);
}

TEST(StoreIo, MountCostsHeadersPlusOneRecord) {
    Rig r(0x40, 1024, 8, 1024, 0x2100);
    ASSERT_EQ(r.store.mount(), Status::NoRecord);
    ASSERT_EQ(r.save(make_items(120, 1)), Status::Ok);
    r.reboot();
    r.dev.reset_stats();
    ASSERT_EQ(r.store.mount(), Status::Ok);
    EXPECT_EQ(r.dev.read_calls(), 8u + 2u) << "8 headers, then the newest record (header + payload)";
}
