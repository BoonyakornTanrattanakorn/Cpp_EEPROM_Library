#include "eeprom/log_ring.hpp"

#include <cstring>

#include "le.hpp"

namespace eeprom {

namespace {

constexpr uint32_t kBlank = 0xFFFFFFFFu;

// Wrap-safe: is `a` older than `b`?
bool seq_older(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }

uint32_t next_after(uint32_t seq) {
    const uint32_t n = seq + 1u;
    return n == kBlank ? 0u : n; // skip the blank marker when the counter wraps
}

} // namespace

LogRing::LogRing(IDevice& dev, uint32_t base, uint16_t entry_size, uint16_t entry_count)
    : dev_(dev), base_(base), entry_size_(entry_size), entry_count_(entry_count) {}

bool LogRing::args_valid() const {
    if (entry_size_ < kMinEntrySize || entry_size_ > kMaxEntrySize || entry_count_ == 0) return false;
    const uint64_t end = static_cast<uint64_t>(base_) + static_cast<uint64_t>(entry_size_) * entry_count_;
    return end <= 0x100000000ull;
}

Status LogRing::read_seq(uint16_t index, uint32_t& seq) {
    uint8_t raw[4];
    if (!dev_.read(entry_addr(index), raw, sizeof(raw))) return Status::IoError;
    seq = detail::load32(raw);
    return Status::Ok;
}

Status LogRing::mount() {
    mounted_ = false;
    count_ = 0;
    head_ = 0;
    next_seq_ = 0;
    if (!args_valid()) return Status::BadArg;

    uint32_t s0;
    Status st = read_seq(0, s0);
    if (st != Status::Ok) return st;
    if (s0 == kBlank) { // nothing written (or cleared)
        mounted_ = true;
        return Status::Ok;
    }

    // Step 1: `used` = number of leading non-blank entries. Blank entries only ever form a suffix
    // (a ring that has not filled yet, or one whose clear() was interrupted).
    uint32_t last;
    st = read_seq(static_cast<uint16_t>(entry_count_ - 1u), last);
    if (st != Status::Ok) return st;
    uint16_t used = entry_count_;
    if (last == kBlank) {
        uint16_t lo = 0; // entry 0 is non-blank
        uint16_t hi = static_cast<uint16_t>(entry_count_ - 1u); // blank
        while (hi - lo > 1) {
            const uint16_t mid = static_cast<uint16_t>((static_cast<uint32_t>(lo) + hi) / 2u);
            uint32_t s;
            st = read_seq(mid, s);
            if (st != Status::Ok) return st;
            if (s == kBlank) {
                hi = mid;
            } else {
                lo = mid;
            }
        }
        used = hi;
    }

    // Step 2: entries 0..p are not older than entry 0; entries after p are older (the ring has
    // wrapped). Find the last such index p among the used entries: that is the newest.
    uint16_t lo = 0;
    uint16_t hi = static_cast<uint16_t>(used - 1u);
    uint32_t newest_seq = 0;
    bool found = false;
    {
        uint32_t s;
        st = read_seq(hi, s);
        if (st != Status::Ok) return st;
        if (!seq_older(s, s0)) { // sorted all the way: the common case of a ring that is not full
            lo = hi;
            newest_seq = s;
            found = true;
        }
    }
    while (!found && lo < hi) {
        const uint16_t mid = static_cast<uint16_t>((static_cast<uint32_t>(lo) + hi + 1u) / 2u);
        uint32_t s;
        st = read_seq(mid, s);
        if (st != Status::Ok) return st;
        if (!seq_older(s, s0)) {
            lo = mid;
        } else {
            hi = static_cast<uint16_t>(mid - 1u);
        }
    }
    const uint16_t newest = lo;
    if (!found) {
        st = read_seq(newest, newest_seq);
        if (st != Status::Ok) return st;
    }

    // A full ring holds every entry. With blanks present, only the run up to `newest` counts:
    // anything after it is stale (left by an interrupted clear) and will be overwritten.
    const uint16_t count = used == entry_count_ ? entry_count_ : static_cast<uint16_t>(newest + 1u);

    count_ = count;
    head_ = static_cast<uint16_t>((newest + 1u) % entry_count_);
    next_seq_ = next_after(newest_seq);
    mounted_ = true;
    return Status::Ok;
}

Status LogRing::append(const void* entry) {
    if (!mounted_) return Status::NotMounted;
    if (entry == nullptr) return Status::BadArg;

    uint8_t buf[kMaxEntrySize];
    std::memcpy(buf, entry, entry_size_);
    detail::store32(buf, next_seq_);
    if (!dev_.write(entry_addr(head_), buf, entry_size_)) return Status::IoError;

    next_seq_ = next_after(next_seq_);
    head_ = static_cast<uint16_t>((head_ + 1u) % entry_count_);
    if (count_ < entry_count_) ++count_;
    return Status::Ok;
}

Status LogRing::read(uint16_t newest_index, void* out) {
    if (!mounted_) return Status::NotMounted;
    if (out == nullptr) return Status::BadArg;
    if (newest_index >= count_) return Status::NotFound;
    const uint16_t pos = static_cast<uint16_t>((static_cast<uint32_t>(head_) + entry_count_ - 1u - newest_index) % entry_count_);
    return dev_.read(entry_addr(pos), static_cast<uint8_t*>(out), entry_size_) ? Status::Ok : Status::IoError;
}

Status LogRing::clear() {
    if (!args_valid()) return Status::BadArg;
    uint8_t blank[64];
    std::memset(blank, 0xFF, sizeof(blank));

    // Blocks of whole entries, from the end of the region down to entry 0. Erasing entry 0 last
    // keeps the ring mountable at every step (it just loses its oldest entries first).
    uint32_t per_block = sizeof(blank) / entry_size_;
    if (per_block == 0) per_block = 1;
    uint32_t end = entry_count_;
    while (end > 0) {
        const uint32_t start = end > per_block ? end - per_block : 0;
        uint32_t addr = entry_addr(static_cast<uint16_t>(start));
        uint32_t remaining = (end - start) * entry_size_;
        while (remaining > 0) {
            const uint32_t n = remaining < sizeof(blank) ? remaining : static_cast<uint32_t>(sizeof(blank));
            if (!dev_.write(addr, blank, n)) {
                mounted_ = false;
                return Status::IoError;
            }
            addr += n;
            remaining -= n;
        }
        end = start;
    }
    mounted_ = true;
    count_ = 0;
    head_ = 0;
    next_seq_ = 0;
    return Status::Ok;
}

} // namespace eeprom
