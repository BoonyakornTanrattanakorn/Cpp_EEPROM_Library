#include "eeprom/status.hpp"

namespace eeprom {

const char* to_string(Status s) {
    switch (s) {
    case Status::Ok: return "Ok";
    case Status::NoRecord: return "NoRecord";
    case Status::IoError: return "IoError";
    case Status::Corrupt: return "Corrupt";
    case Status::TooLarge: return "TooLarge";
    case Status::BadArg: return "BadArg";
    case Status::NotMounted: return "NotMounted";
    case Status::NotFound: return "NotFound";
    case Status::VerifyFailed: return "VerifyFailed";
    }
    return "?";
}

} // namespace eeprom
