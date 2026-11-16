# C++ EEPROM Library

A wear-levelled key-value store and log ring for small serial EEPROMs (e.g. AT24-series I²C parts), written in C++17 for microcontrollers.

> **Status:** early development. There is no usable code yet; the first release is planned as v0.1.0.

## Goals

- **Wear levelling** across the EEPROM, so frequently changed values don't wear out one page.
- **Versioned configuration records** with integrity checks, so a power loss mid-write never corrupts the stored config.
- **Log ring** for append-only event records.
- **No dependencies:** no RTOS, HAL, heap or exceptions. You give it a small device interface (read, write, page size) for your EEPROM.
- **Portable C++17** in the `eeprom` namespace, with host unit tests.

## Using it

Add it as a git submodule and from CMake:

```cmake
add_subdirectory(lib/Cpp_EEPROM_Library)
target_link_libraries(your_target PRIVATE eeprom)
```

## Licence

MIT. See [LICENSE](LICENSE).
