#include "eeprom/writer.hpp"

#include <cstring>

#include "le.hpp"

namespace eeprom {

uint32_t get_u32(const uint8_t* v) { return detail::load32(v); }
int32_t get_i32(const uint8_t* v) { return static_cast<int32_t>(detail::load32(v)); }
float get_f32(const uint8_t* v) {
    const uint32_t bits = detail::load32(v);
    float f;
    static_assert(sizeof(f) == sizeof(bits), "float must be 32 bits");
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

Status Writer::put(uint16_t id, Type t, const uint8_t* value, size_t len) {
    if (status_ != Status::Ok) return status_;
    if (len > 255) return status_ = Status::BadArg;
    if (cap_ - size_ < 4 + len) return status_ = Status::TooLarge;
    uint8_t* p = buf_ + size_;
    detail::store16(p, id);
    p[2] = static_cast<uint8_t>(t);
    p[3] = static_cast<uint8_t>(len);
    if (len != 0) std::memcpy(p + 4, value, len);
    size_ += 4 + len;
    return Status::Ok;
}

Status Writer::put_u32(uint16_t id, uint32_t v) {
    uint8_t b[4];
    detail::store32(b, v);
    return put(id, Type::U32, b, 4);
}

Status Writer::put_i32(uint16_t id, int32_t v) {
    uint8_t b[4];
    detail::store32(b, static_cast<uint32_t>(v));
    return put(id, Type::I32, b, 4);
}

Status Writer::put_f32(uint16_t id, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    uint8_t b[4];
    detail::store32(b, bits);
    return put(id, Type::F32, b, 4);
}

Status Writer::put_bool(uint16_t id, bool v) {
    const uint8_t b = v ? 1 : 0;
    return put(id, Type::BOOL, &b, 1);
}

Status Writer::put_blob(uint16_t id, const uint8_t* data, size_t len) {
    return put(id, Type::BLOB, data, len);
}

} // namespace eeprom
