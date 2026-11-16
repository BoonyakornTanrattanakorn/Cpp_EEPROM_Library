#pragma once

#include <cstdint>

namespace eeprom {

/// Result of every fallible call. Nothing in this library throws.
enum class Status : uint8_t {
    Ok = 0,
    NoRecord,    ///< Store: no valid record in the region (blank or all slots corrupt). Not a fault on first boot.
    IoError,     ///< The device read or write returned false.
    Corrupt,     ///< A record failed its CRC or has an inconsistent structure.
    TooLarge,    ///< Payload or record does not fit the scratch buffer or slot.
    BadArg,      ///< Invalid region, size or argument.
    NotMounted,  ///< mount() has not been called (or failed).
    NotFound,    ///< LogRing::read: index beyond count().
    VerifyFailed ///< Read-back after a write did not match.
};

constexpr bool ok(Status s) { return s == Status::Ok; }

const char* to_string(Status s);

} // namespace eeprom
