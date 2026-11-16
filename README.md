# C++ EEPROM Library

A wear-levelled, power-loss-safe record store and a log ring for small serial EEPROMs (AT24-series I²C parts and similar), in C++17 for microcontrollers.

- **No dependencies, no heap, no exceptions, no RTTI.** You supply one small device interface and a scratch buffer.
- Namespace `eeprom`. Builds for Cortex-M4 (`arm-none-eabi-g++`) and for the host. The whole library is about 2.4 kB of code at `-Os`.
- Tested on the host against a RAM fake that cuts power at **every byte** of a save.

| Class | Purpose |
|---|---|
| `Store` | Versioned key-value record, rotated through `N` slots. For settings, calibration and counters. |
| `LogRing` | Fixed-size entries, oldest overwritten. For fault logs. |
| `IDevice` | The EEPROM, implemented by you. |

## Record format (`Store`)

One record per slot, little-endian. The CRC covers `sequence` to the end of the payload, with the `crc32` field itself read as zero.

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | u32 | `magic` | Region identifier (`Region::magic`); other regions' records are ignored |
| 4 | u32 | `sequence` | Increases by 1 on every save; compared wrap-safe |
| 8 | u16 | `layout` | Application layout version (bump only for breaking changes) |
| 10 | u16 | `length` | Payload bytes |
| 12 | u32 | `crc32` | CRC-32 (zlib/IEEE) over bytes 4..16+length, field 12..15 zeroed |
| 16 | | payload | Entries, back to back; the rest of the slot is not written |

Entry: `id u16 | type u8 | len u8 | value[len]`. Types: `U32`=0, `I32`=1, `F32`=2 (4 bytes), `BOOL`=3 (1 byte), `BLOB`=4 (0..255 bytes).

## How it works

- **Wear levelling.** Each `save()` writes the slot after the newest one, so writes rotate evenly over `slot_count` slots. Over 10,000 saves into 8 slots every slot is written exactly 1,250 times.
- **Power-loss safety.** The newest valid record is never overwritten: the target is always the oldest slot. The record is written with one `IDevice::write`, so a cut leaves a slot that fails its CRC (or, if the cut came before the first byte changed, the old record). `mount()` then picks the newest slot with a valid CRC. If the newest is half-written, it is skipped and the previous record is returned. After any cut the result is the old record or the new one, intact, never a mix.
- **Choosing the record.** `mount()` reads all slot headers (16 bytes each), then checks CRCs from the highest sequence downwards and stops at the first good one. Sequence numbers are compared as `int32_t(a - b)`, so they may wrap past 2³².
- **Version tolerance.** Entries are keyed by stable ids. On `load()`, ids the application does not know are ignored; ids missing from the record are never reported, so the application keeps their defaults; a value the application rejects is reported as out of range. Adding or removing parameters needs no migration.
- **Integrity.** CRC-32 over the whole record. `set_verify_after_write(true)` also reads the record back after each save.
- **`LogRing`.** The first 4 bytes of every entry are a sequence number the ring writes (`0xFFFFFFFF` = blank). `mount()` finds the newest entry by binary search (about 13 small reads for 512 entries). Entries carry no CRC and are written in one `write`, so choose an entry size that divides the EEPROM page size: a page write is the unit that real I²C parts make atomic.

## API summary

```cpp
struct IDevice {                       // you implement this
    virtual bool read(uint32_t addr, uint8_t* dst, size_t len) = 0;
    virtual bool write(uint32_t addr, const uint8_t* src, size_t len) = 0;  // split pages, wait for the write cycle
};

struct Region { uint32_t base; uint32_t slot_size; uint16_t slot_count; uint32_t magic; };

class Store {
    Store(IDevice&, Region, uint8_t* scratch, size_t scratch_len, Crc32Fn = &soft_crc32);
    Status mount();   // Ok, NoRecord (blank/all corrupt: use defaults, save() still works), or an error
    Status load(EntryResult (*on_entry)(void* ctx, uint16_t id, Type, const uint8_t* v, uint8_t len),
                void* ctx, uint16_t* layout_out, LoadReport* report = nullptr);
    Status save(Status (*emit_all)(void* ctx, Writer&), void* ctx, uint16_t layout);
    void set_verify_after_write(bool);
    uint32_t sequence() const;  uint16_t active_slot() const;  uint16_t next_slot() const;  bool has_record() const;
};

class Writer {                         // passed to emit_all
    Status put_u32(id, v); put_i32(id, v); put_f32(id, v); put_bool(id, v); put_blob(id, data, len);
};
// get_u32 / get_i32 / get_f32 decode the values handed to on_entry.

class LogRing {
    LogRing(IDevice&, uint32_t base, uint16_t entry_size, uint16_t entry_count);  // entry_size 4..128
    Status mount();  Status append(const void* entry);
    Status read(uint16_t newest_index, void* out);   // 0 = newest
    uint16_t count() const;  Status clear();
};
```

`scratch` must hold the largest record (16 bytes of header plus the payload); it may be shared by Stores that are not used at the same time. The CRC is pluggable: any function with the signature `uint32_t(const uint8_t*, size_t)` that computes the same CRC-32 (for example one that drives a hardware CRC unit).

## Example

```cpp
#include "eeprom/eeprom.hpp"

class MyEeprom final : public eeprom::IDevice {          // 24C256: 64-byte pages
public:
    bool read(uint32_t addr, uint8_t* dst, size_t len) override {
        return i2c_mem_read(0x50, addr, dst, len);
    }
    bool write(uint32_t addr, const uint8_t* src, size_t len) override {
        while (len > 0) {                                  // split at page boundaries
            size_t n = 64 - (addr % 64);
            if (n > len) n = len;
            if (!i2c_mem_write(0x50, addr, src, n)) return false;
            wait_for_ack(0x50);                            // internal write cycle
            addr += static_cast<uint32_t>(n); src += n; len -= n;
        }
        return true;
    }
};

struct Settings { uint32_t brightness = 50; float gain = 1.0f; };

static uint8_t scratch[1024];

eeprom::Status save_settings(eeprom::Store& store, const Settings& s) {
    return store.save([](void* c, eeprom::Writer& w) {
        auto& cfg = *static_cast<const Settings*>(c);
        eeprom::Status st = w.put_u32(1, cfg.brightness);
        return st == eeprom::Status::Ok ? w.put_f32(2, cfg.gain) : st;
    }, const_cast<Settings*>(&s), /*layout=*/1);
}

void boot(MyEeprom& dev, Settings& s) {
    static eeprom::Store store(dev, {0x0040, 1024, 8, 0x4F514C53}, scratch, sizeof(scratch));
    if (store.mount() == eeprom::Status::Ok) {             // NoRecord: keep the defaults
        store.load([](void* c, uint16_t id, eeprom::Type, const uint8_t* v, uint8_t) {
            auto& cfg = *static_cast<Settings*>(c);
            switch (id) {
            case 1:
                if (eeprom::get_u32(v) > 100) return eeprom::EntryResult::OutOfRange;
                cfg.brightness = eeprom::get_u32(v);
                return eeprom::EntryResult::Applied;
            case 2:
                cfg.gain = eeprom::get_f32(v);
                return eeprom::EntryResult::Applied;
            default:
                return eeprom::EntryResult::Unknown;
            }
        }, &s, nullptr);
    }
}
```

## Build and test

As a subproject (git submodule):

```cmake
add_subdirectory(lib/Cpp_EEPROM_Library)      # tests are off by default here
target_link_libraries(your_target PRIVATE eeprom::eeprom)
```

Host tests (GoogleTest v1.18.0 is fetched; needs CMake 3.16+ and network on the first configure):

```
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure      # or build/test/eeprom_tests
```

The suite includes a power cut at every byte offset of a save (about 43,000 cut points over six region geometries, with sequence numbers both far from and near the 2³² wrap), a 10,000-save wear test that prints writes per slot, CRC and single-bit-flip detection, and `LogRing` mount after wrap and after power cuts. `test/fake_device.hpp` is the reusable RAM device with power-cut and bit-flip injection.

The library sources compile cleanly with `-Wall -Wextra -Wshadow -Wconversion -fno-exceptions -fno-rtti`. Design decisions and deviations are in [doc/design-notes.md](doc/design-notes.md).

## Licence

MIT. See [LICENSE](LICENSE).
