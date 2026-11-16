#pragma once

// Shared helpers for the Store tests: a bundle of device + region + scratch, a callback pair
// that saves/loads a list of u32 parameters, and a raw record writer for crafting slots.

#include <cstdint>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#include "eeprom/eeprom.hpp"
#include "fake_device.hpp"

namespace eeprom::test {

using Items = std::vector<std::pair<uint16_t, uint32_t>>;

inline Status emit_items(void* ctx, Writer& w) {
    for (const auto& it : *static_cast<const Items*>(ctx)) {
        const Status s = w.put_u32(it.first, it.second);
        if (s != Status::Ok) return s;
    }
    return Status::Ok;
}

inline EntryResult collect_items(void* ctx, uint16_t id, Type t, const uint8_t* v, uint8_t len) {
    if (t == Type::U32 && len == 4) static_cast<Items*>(ctx)->emplace_back(id, get_u32(v));
    return EntryResult::Applied;
}

/// `count` parameters with ids 1..count and values derived from `salt`.
inline Items make_items(uint16_t count, uint32_t salt) {
    Items items;
    for (uint16_t i = 1; i <= count; ++i) items.emplace_back(i, salt * 1000003u + i * 7919u);
    return items;
}

constexpr uint32_t kMagic = 0x4F514C43u; // "CLQO" little-endian, arbitrary

/// Device + region + scratch, plus a Store that can be re-created to simulate a reboot.
struct Rig {
    FakeDevice dev;
    Region region;
    std::vector<uint8_t> scratch;
    Store store;

    Rig(uint32_t base, uint32_t slot_size, uint16_t slot_count, size_t scratch_len, size_t capacity = 0x2000)
        : dev(capacity), region{base, slot_size, slot_count, kMagic}, scratch(scratch_len),
          store(dev, region, scratch.data(), scratch.size()) {}

    /// New Store object on the same memory: what a reboot looks like to the library.
    void reboot() {
        dev.restore_power();
        store.~Store();
        new (&store) Store(dev, region, scratch.data(), scratch.size());
    }

    Status save(const Items& items, uint16_t layout = 1) {
        return store.save(&emit_items, const_cast<Items*>(&items), layout);
    }

    Status load(Items& out, uint16_t* layout = nullptr, LoadReport* rep = nullptr) {
        out.clear();
        return store.load(&collect_items, &out, layout, rep);
    }
};

/// Writes a complete, valid record into `slot` (bypassing Store), e.g. to start at a chosen sequence.
inline void write_record(FakeDevice& dev, const Region& r, uint16_t slot, uint32_t seq, uint16_t layout,
                         const std::vector<uint8_t>& payload, uint32_t magic_override = 0) {
    std::vector<uint8_t> rec(16 + payload.size());
    auto put32 = [&](size_t off, uint32_t v) {
        for (int i = 0; i < 4; ++i) rec[off + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
    };
    put32(0, magic_override != 0 ? magic_override : r.magic);
    put32(4, seq);
    rec[8] = static_cast<uint8_t>(layout);
    rec[9] = static_cast<uint8_t>(layout >> 8);
    rec[10] = static_cast<uint8_t>(payload.size());
    rec[11] = static_cast<uint8_t>(payload.size() >> 8);
    put32(12, 0);
    if (!payload.empty()) std::memcpy(rec.data() + 16, payload.data(), payload.size());
    put32(12, soft_crc32(rec.data() + 4, rec.size() - 4));
    dev.restore_power();
    dev.write(r.base + static_cast<uint32_t>(slot) * r.slot_size, rec.data(), rec.size());
}

/// Payload bytes for one u32 entry.
inline std::vector<uint8_t> u32_entry(uint16_t id, uint32_t v) {
    std::vector<uint8_t> p(8);
    p[0] = static_cast<uint8_t>(id);
    p[1] = static_cast<uint8_t>(id >> 8);
    p[2] = static_cast<uint8_t>(Type::U32);
    p[3] = 4;
    for (int i = 0; i < 4; ++i) p[4 + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
    return p;
}

} // namespace eeprom::test
