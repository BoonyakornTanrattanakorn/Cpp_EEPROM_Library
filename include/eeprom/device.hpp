#pragma once

#include <cstddef>
#include <cstdint>

namespace eeprom {

/// The EEPROM, as the library sees it. Implemented by the user.
///
/// `write` must accept any address range inside the device and write it in ascending address order.
/// Splitting at page boundaries and waiting for the internal write cycle (ACK polling) are the
/// implementation's job; the library calls `write` with whole records.
/// Both calls return false on any bus or range error.
class IDevice {
public:
    virtual bool read(uint32_t addr, uint8_t* dst, size_t len) = 0;
    virtual bool write(uint32_t addr, const uint8_t* src, size_t len) = 0;

protected:
    // Non-virtual and protected: nobody deletes through this interface, and it keeps the
    // deleting destructor (and so operator delete) out of the vtable on bare-metal targets.
    ~IDevice() = default;
};

} // namespace eeprom
