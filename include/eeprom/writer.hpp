#pragma once

#include <cstddef>
#include <cstdint>

#include "eeprom/status.hpp"

namespace eeprom {

/// Value type of a stored entry.
enum class Type : uint8_t { U32 = 0, I32 = 1, F32 = 2, BOOL = 3, BLOB = 4 };

/// Decoders for the values handed to the `load` callback (little-endian, as stored).
uint32_t get_u32(const uint8_t* v);
int32_t get_i32(const uint8_t* v);
float get_f32(const uint8_t* v);

/// Appends `id | type | len | value` entries to a payload buffer. Handed to the `save` callback.
/// The first failure is sticky: later puts keep returning it, and `save` returns it too.
class Writer {
public:
    Writer(uint8_t* buf, size_t capacity) : buf_(buf), cap_(capacity) {}

    Status put_u32(uint16_t id, uint32_t v);
    Status put_i32(uint16_t id, int32_t v);
    Status put_f32(uint16_t id, float v);
    Status put_bool(uint16_t id, bool v);
    /// `len` must be 0..255.
    Status put_blob(uint16_t id, const uint8_t* data, size_t len);

    size_t size() const { return size_; }
    Status status() const { return status_; }

private:
    Status put(uint16_t id, Type t, const uint8_t* value, size_t len);

    uint8_t* buf_;
    size_t cap_;
    size_t size_ = 0;
    Status status_ = Status::Ok;
};

} // namespace eeprom
