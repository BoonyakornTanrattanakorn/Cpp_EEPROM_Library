#pragma once

#include <cstddef>
#include <cstdint>

#include "eeprom/crc32.hpp"
#include "eeprom/device.hpp"
#include "eeprom/status.hpp"
#include "eeprom/writer.hpp"

namespace eeprom {

/// Where a Store lives on the device: `slot_count` slots of `slot_size` bytes starting at `base`.
/// `magic` identifies the region (use a different value per Store on one device).
struct Region {
    uint32_t base;
    uint32_t slot_size;
    uint16_t slot_count;
    uint32_t magic;
};

/// What the application's `load` callback did with an entry.
enum class EntryResult : uint8_t {
    Applied,   ///< Value accepted.
    Unknown,   ///< Id not known to this firmware version (a removed parameter): ignored.
    OutOfRange ///< Id known but the value is invalid: the application keeps the default.
};

/// Counts from one `load`. Parameters absent from the record are simply never reported:
/// the application keeps their defaults.
struct LoadReport {
    uint16_t entries = 0; ///< Entries in the record.
    uint16_t applied = 0;
    uint16_t unknown = 0; ///< Unknown ids, plus entries with an unknown type.
    uint16_t out_of_range = 0;
    uint16_t malformed = 0; ///< Known type with a wrong length: skipped, callback not called.
};

/// Wear-levelled, power-loss-safe record store (see the README for the format).
///
/// Not thread-safe; one Store owns its region. `scratch` holds one whole record while saving,
/// mounting and loading: it must be larger than the largest payload by at least 16 bytes, and
/// may be shared between Stores that are not used concurrently.
class Store {
public:
    static constexpr size_t kHeaderSize = 16;

    using EntryFn = EntryResult (*)(void* ctx, uint16_t id, Type t, const uint8_t* value, uint8_t len);
    using EmitFn = Status (*)(void* ctx, Writer& w);

    Store(IDevice& dev, Region r, uint8_t* scratch, size_t scratch_len, Crc32Fn crc32 = &soft_crc32);

    /// Reads every slot header, then verifies candidates newest first and keeps the newest
    /// whose CRC is good. Returns Ok, NoRecord (nothing valid; save() still works) or an error.
    Status mount();

    /// Reads the active record again, verifies it and calls `on_entry` per entry.
    /// `layout_version_out` and `report` may be null.
    Status load(EntryFn on_entry, void* ctx, uint16_t* layout_version_out, LoadReport* report = nullptr);

    /// Runs `emit_all` to serialise the payload, then writes it to the slot after the newest one.
    /// The newest valid record is never touched. On failure nothing changes in the Store.
    Status save(EmitFn emit_all, void* ctx, uint16_t layout_version);

    /// Optional: read the record back after every save and compare (Status::VerifyFailed).
    void set_verify_after_write(bool enable) { verify_ = enable; }

    bool has_record() const { return has_record_; }
    uint32_t sequence() const { return sequence_; }
    uint16_t active_slot() const { return active_; }
    /// The slot the next save() will write.
    uint16_t next_slot() const;

private:
    struct Header {
        uint32_t magic;
        uint32_t sequence;
        uint16_t layout;
        uint16_t length;
        uint32_t crc;
    };

    bool region_valid() const;
    uint32_t slot_addr(uint16_t slot) const { return region_.base + static_cast<uint32_t>(slot) * region_.slot_size; }
    size_t max_payload() const;
    bool plausible(const Header& h) const;
    Status read_header(uint16_t slot, Header& h);
    /// Reads the whole record of `slot` into scratch and checks its CRC. Ok = valid, Corrupt = bad CRC.
    Status read_verified(uint16_t slot, Header& h);

    IDevice& dev_;
    Region region_;
    uint8_t* scratch_;
    size_t scratch_len_;
    Crc32Fn crc_;
    bool verify_ = false;
    bool mounted_ = false;
    bool has_record_ = false;
    uint16_t active_ = 0;
    uint32_t sequence_ = 0;
};

} // namespace eeprom
