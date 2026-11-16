#pragma once

#include <cstddef>
#include <cstdint>

#include "eeprom/device.hpp"
#include "eeprom/status.hpp"

namespace eeprom {

/// Ring of fixed-size entries; once full, the oldest entry is overwritten.
///
/// The first 4 bytes of every entry are a little-endian sequence number owned by the ring:
/// `append` overwrites them, `read` returns them. 0xFFFFFFFF marks a blank (erased) entry.
/// Entries carry no CRC (the layout is yours); choose `entry_size` so that entries do not
/// straddle EEPROM pages, since a page write is the unit that is atomic on real parts.
class LogRing {
public:
    static constexpr uint16_t kMinEntrySize = 4;
    static constexpr uint16_t kMaxEntrySize = 128;

    LogRing(IDevice& dev, uint32_t base, uint16_t entry_size, uint16_t entry_count);

    /// Binary search for the newest entry: about 2 + log2(entry_count) small reads.
    Status mount();

    /// Stores `entry` (entry_size bytes) at the next position, stamping its sequence number.
    Status append(const void* entry);

    /// `newest_index` 0 is the newest entry. Copies entry_size bytes to `out`.
    Status read(uint16_t newest_index, void* out);

    uint16_t count() const { return count_; }
    /// Sequence number the next append will use.
    uint32_t next_sequence() const { return next_seq_; }

    /// Blanks the whole region, last entry first, so an interrupted clear leaves a valid ring.
    Status clear();

private:
    bool args_valid() const;
    uint32_t entry_addr(uint16_t index) const { return base_ + static_cast<uint32_t>(index) * entry_size_; }
    Status read_seq(uint16_t index, uint32_t& seq);

    IDevice& dev_;
    uint32_t base_;
    uint16_t entry_size_;
    uint16_t entry_count_;
    bool mounted_ = false;
    uint16_t count_ = 0;
    uint16_t head_ = 0; ///< Index of the next write.
    uint32_t next_seq_ = 0;
};

} // namespace eeprom
