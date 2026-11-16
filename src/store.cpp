#include "eeprom/store.hpp"

#include <cstring>

#include "le.hpp"

namespace eeprom {

namespace {

constexpr uint32_t kMaxLength = 0xFFFFu; // payload length is a u16

// Wrap-safe order on sequence numbers: valid while two live sequences differ by less than 2^31.
bool seq_older(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }

// Is slot A (seq sa) older than slot B (seq sb)? Equal sequences (never written by us) order by slot.
bool older(uint32_t sa, uint16_t slot_a, uint32_t sb, uint16_t slot_b) {
    return sa != sb ? seq_older(sa, sb) : slot_a < slot_b;
}

} // namespace

Store::Store(IDevice& dev, Region r, uint8_t* scratch, size_t scratch_len, Crc32Fn crc32)
    : dev_(dev), region_(r), scratch_(scratch), scratch_len_(scratch_len), crc_(crc32) {}

bool Store::region_valid() const {
    if (scratch_ == nullptr || crc_ == nullptr) return false;
    if (region_.slot_count < 2) return false;
    if (region_.slot_size <= kHeaderSize || region_.slot_size - kHeaderSize > kMaxLength) return false;
    if (scratch_len_ <= kHeaderSize) return false;
    const uint64_t end = static_cast<uint64_t>(region_.base) + static_cast<uint64_t>(region_.slot_size) * region_.slot_count;
    return end <= 0x100000000ull;
}

size_t Store::max_payload() const {
    const size_t room = region_.slot_size < scratch_len_ ? region_.slot_size : scratch_len_;
    const size_t payload = room - kHeaderSize;
    return payload > kMaxLength ? kMaxLength : payload;
}

bool Store::plausible(const Header& h) const {
    return h.magic == region_.magic && h.length <= region_.slot_size - kHeaderSize;
}

static void parse_header(const uint8_t* p, uint32_t& magic, uint32_t& seq, uint16_t& layout, uint16_t& length, uint32_t& crc) {
    magic = detail::load32(p);
    seq = detail::load32(p + 4);
    layout = detail::load16(p + 8);
    length = detail::load16(p + 10);
    crc = detail::load32(p + 12);
}

Status Store::read_header(uint16_t slot, Header& h) {
    uint8_t raw[kHeaderSize];
    if (!dev_.read(slot_addr(slot), raw, sizeof(raw))) return Status::IoError;
    parse_header(raw, h.magic, h.sequence, h.layout, h.length, h.crc);
    return Status::Ok;
}

Status Store::read_verified(uint16_t slot, Header& h) {
    // Record bytes are: magic | sequence | layout | length | crc | payload. The CRC covers
    // sequence..payload; the crc field itself is treated as zero while computing.
    if (!dev_.read(slot_addr(slot), scratch_, kHeaderSize)) return Status::IoError;
    parse_header(scratch_, h.magic, h.sequence, h.layout, h.length, h.crc);
    if (!plausible(h)) return Status::Corrupt;
    const size_t total = kHeaderSize + h.length;
    if (total > scratch_len_) return Status::TooLarge;
    if (h.length != 0 && !dev_.read(slot_addr(slot) + kHeaderSize, scratch_ + kHeaderSize, h.length)) return Status::IoError;
    detail::store32(scratch_ + 12, 0);
    const uint32_t crc = crc_(scratch_ + 4, total - 4);
    detail::store32(scratch_ + 12, h.crc);
    return crc == h.crc ? Status::Ok : Status::Corrupt;
}

Status Store::mount() {
    mounted_ = false;
    has_record_ = false;
    if (!region_valid()) return Status::BadArg;

    bool have_bound = false; // candidates must be older than (bound_seq, bound_slot)
    uint32_t bound_seq = 0;
    uint16_t bound_slot = 0;

    for (;;) {
        bool found = false;
        uint32_t best_seq = 0;
        uint16_t best_slot = 0;
        for (uint16_t s = 0; s < region_.slot_count; ++s) {
            Header h;
            const Status st = read_header(s, h);
            if (st != Status::Ok) return st;
            if (!plausible(h)) continue;
            if (have_bound && !older(h.sequence, s, bound_seq, bound_slot)) continue;
            if (!found || older(best_seq, best_slot, h.sequence, s)) {
                found = true;
                best_seq = h.sequence;
                best_slot = s;
            }
        }
        if (!found) break;

        Header h;
        const Status st = read_verified(best_slot, h);
        if (st == Status::Ok) {
            has_record_ = true;
            active_ = best_slot;
            sequence_ = h.sequence;
            mounted_ = true;
            return Status::Ok;
        }
        if (st != Status::Corrupt) return st; // IoError, TooLarge: not a bad record, a bad setup
        have_bound = true;
        bound_seq = best_seq;
        bound_slot = best_slot;
    }
    mounted_ = true;
    return Status::NoRecord;
}

uint16_t Store::next_slot() const {
    return has_record_ ? static_cast<uint16_t>((active_ + 1u) % region_.slot_count) : 0;
}

Status Store::load(EntryFn on_entry, void* ctx, uint16_t* layout_version_out, LoadReport* report) {
    if (!mounted_) return Status::NotMounted;
    if (!has_record_) return Status::NoRecord;
    if (on_entry == nullptr) return Status::BadArg;

    Header h;
    const Status st = read_verified(active_, h);
    if (st != Status::Ok) return st; // Corrupt here means the record rotted since mount()

    LoadReport local;
    LoadReport& rep = report != nullptr ? *report : local;
    rep = LoadReport();
    if (layout_version_out != nullptr) *layout_version_out = h.layout;

    const uint8_t* p = scratch_ + kHeaderSize;
    size_t pos = 0;
    while (pos < h.length) {
        if (h.length - pos < 4) return Status::Corrupt;
        const uint16_t id = detail::load16(p + pos);
        const uint8_t type = p[pos + 2];
        const uint8_t len = p[pos + 3];
        if (h.length - pos - 4 < len) return Status::Corrupt;
        const uint8_t* value = p + pos + 4;
        pos += 4u + len;
        ++rep.entries;

        size_t expect;
        switch (static_cast<Type>(type)) {
        case Type::U32:
        case Type::I32:
        case Type::F32: expect = 4; break;
        case Type::BOOL: expect = 1; break;
        case Type::BLOB: expect = len; break;
        default: ++rep.unknown; continue; // a type from a newer library version
        }
        if (len != expect) {
            ++rep.malformed;
            continue;
        }
        switch (on_entry(ctx, id, static_cast<Type>(type), value, len)) {
        case EntryResult::Applied: ++rep.applied; break;
        case EntryResult::Unknown: ++rep.unknown; break;
        case EntryResult::OutOfRange: ++rep.out_of_range; break;
        }
    }
    return Status::Ok;
}

Status Store::save(EmitFn emit_all, void* ctx, uint16_t layout_version) {
    if (!mounted_) return Status::NotMounted;
    if (emit_all == nullptr) return Status::BadArg;

    Writer w(scratch_ + kHeaderSize, max_payload());
    Status st = emit_all(ctx, w);
    if (st != Status::Ok) return st;
    if (w.status() != Status::Ok) return w.status();

    const uint16_t length = static_cast<uint16_t>(w.size());
    const uint16_t slot = next_slot();
    const uint32_t seq = has_record_ ? sequence_ + 1u : 1u;
    const size_t total = kHeaderSize + length;

    detail::store32(scratch_, region_.magic);
    detail::store32(scratch_ + 4, seq);
    detail::store16(scratch_ + 8, layout_version);
    detail::store16(scratch_ + 10, length);
    detail::store32(scratch_ + 12, 0);
    detail::store32(scratch_ + 12, crc_(scratch_ + 4, total - 4));

    // One write of the whole record. The slot holds the oldest record, so a cut anywhere in
    // this write leaves the newest record untouched and the half-written slot fails its CRC.
    const uint32_t addr = slot_addr(slot);
    if (!dev_.write(addr, scratch_, total)) return Status::IoError;

    if (verify_) {
        uint8_t chunk[32];
        for (size_t off = 0; off < total; off += sizeof(chunk)) {
            const size_t n = total - off < sizeof(chunk) ? total - off : sizeof(chunk);
            if (!dev_.read(addr + static_cast<uint32_t>(off), chunk, n)) return Status::IoError;
            if (std::memcmp(chunk, scratch_ + off, n) != 0) return Status::VerifyFailed;
        }
    }

    has_record_ = true;
    active_ = slot;
    sequence_ = seq;
    return Status::Ok;
}

} // namespace eeprom
