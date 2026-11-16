#pragma once

// RAM-backed EEPROM for host tests, with fault injection:
//  - power cut: the device stops accepting bytes after N more bytes have been written
//    (the write call that crosses the limit stores the bytes up to it, then fails);
//  - bit flips: in stored memory, or in one byte as it is written.
// Blank EEPROM reads 0xFF. It also counts bytes, write calls and page write cycles.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "eeprom/device.hpp"

namespace eeprom::test {

class FakeDevice final : public eeprom::IDevice {
public:
    explicit FakeDevice(size_t capacity, size_t page_size = 64)
        : mem_(capacity, 0xFF), page_size_(page_size), page_writes_((capacity + page_size - 1) / page_size, 0) {}

    bool read(uint32_t addr, uint8_t* dst, size_t len) override {
        if (!powered_ || !in_range(addr, len)) return false;
        std::memcpy(dst, mem_.data() + addr, len);
        ++read_calls_;
        return true;
    }

    bool write(uint32_t addr, const uint8_t* src, size_t len) override {
        if (!powered_ || !in_range(addr, len)) return false;
        ++write_calls_;
        size_t n = len;
        bool cut = false;
        if (cut_armed_ && cut_remaining_ < len) {
            n = cut_remaining_;
            cut = true;
        }
        for (size_t i = 0; i < n; ++i) {
            uint8_t b = src[i];
            if (flip_armed_ && flip_countdown_ == 0) {
                b = static_cast<uint8_t>(b ^ flip_mask_);
                flip_armed_ = false;
            } else if (flip_armed_) {
                --flip_countdown_;
            }
            mem_[addr + i] = b;
        }
        bytes_written_ += n;
        if (n > 0) { // one internal write cycle per page touched, as on a real part
            for (size_t p = addr / page_size_; p <= (addr + n - 1) / page_size_; ++p) ++page_writes_[p];
        }
        if (cut_armed_) cut_remaining_ -= n;
        if (cut) {
            powered_ = false;
            cut_armed_ = false;
            return false;
        }
        return true;
    }

    // --- fault injection ---
    /// Power fails after `bytes` more bytes have been written (0: the next write stores nothing).
    void cut_after(size_t bytes) {
        cut_armed_ = true;
        cut_remaining_ = bytes;
    }
    /// "Reboot": power is back; memory keeps whatever was written.
    void restore_power() {
        powered_ = true;
        cut_armed_ = false;
    }
    bool powered() const { return powered_; }
    void power_off() { powered_ = false; }

    /// Flips one stored bit now.
    void flip_bit(uint32_t addr, unsigned bit) { mem_.at(addr) = static_cast<uint8_t>(mem_.at(addr) ^ (1u << bit)); }
    /// The `nth` byte (0-based) written from now on is stored with `mask` XORed in.
    void flip_on_write(size_t nth, uint8_t mask) {
        flip_armed_ = true;
        flip_countdown_ = nth;
        flip_mask_ = mask;
    }

    // --- inspection ---
    std::vector<uint8_t>& memory() { return mem_; }
    const std::vector<uint8_t>& memory() const { return mem_; }
    size_t capacity() const { return mem_.size(); }
    size_t page_size() const { return page_size_; }
    size_t bytes_written() const { return bytes_written_; }
    size_t write_calls() const { return write_calls_; }
    size_t read_calls() const { return read_calls_; }
    size_t page_writes(size_t page) const { return page_writes_.at(page); }
    size_t page_count() const { return page_writes_.size(); }
    void reset_stats() {
        bytes_written_ = write_calls_ = read_calls_ = 0;
        std::fill(page_writes_.begin(), page_writes_.end(), size_t{0});
    }

private:
    bool in_range(uint32_t addr, size_t len) const { return static_cast<uint64_t>(addr) + len <= mem_.size(); }

    std::vector<uint8_t> mem_;
    size_t page_size_;
    std::vector<size_t> page_writes_;
    bool powered_ = true;
    bool cut_armed_ = false;
    size_t cut_remaining_ = 0;
    bool flip_armed_ = false;
    size_t flip_countdown_ = 0;
    uint8_t flip_mask_ = 0;
    size_t bytes_written_ = 0;
    size_t write_calls_ = 0;
    size_t read_calls_ = 0;
};

} // namespace eeprom::test
