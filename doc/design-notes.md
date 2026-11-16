# Design notes

Deviations from, and clarifications of, the firmware design (`storage.md` §2 in the LaserDigitalController firmware repository), with the reason for each.

## `IDevice`

1. **No `page_size()` or `capacity()`.** The design gives `IDevice` only `read` and `write`, and puts page splitting and ACK polling in the firmware's `At24Storage`. The library therefore issues one `write` per record or entry and never splits at page boundaries. A device wrapper must write the range in ascending address order (the power-cut argument depends on it).
2. **Protected non-virtual destructor** instead of `virtual ~IDevice() = default`. Nothing deletes through the interface, and a virtual destructor puts the deleting destructor (so `operator delete`) into the vtable, which pulls heap code into a bare-metal link.

## `Store`

3. **CRC range.** The design says the CRC covers `sequence..payload` but the `crc32` field sits inside that span, and the pluggable function (`uint32_t(const uint8_t*, size_t)`, not incremental) takes one contiguous buffer. The library reads the `crc32` field as zero and computes over bytes `4 .. 16+length` in one call. A hardware CRC unit can be used as is.
4. **`on_entry` returns an `EntryResult`** (`Applied`, `Unknown`, `OutOfRange`). The design says an out-of-range value is "replaced by its default and reported", but only the application knows the valid range, so the callback decides and the library counts. `load` takes an optional trailing `LoadReport*` (entries, applied, unknown, out of range, malformed). Missing ids are never reported: the application keeps its defaults.
5. **`Status`** is defined here (the design left it open): `Ok, NoRecord, IoError, Corrupt, TooLarge, BadArg, NotMounted, NotFound, VerifyFailed`. `mount()` returns `NoRecord` for blank or fully corrupt regions; it is not an I/O fault, and `save()` works afterwards (first record: slot 0, sequence 1).
6. **`mount()` reads headers first.** The design text says every slot header is read and its CRC checked. Checking every CRC would read the whole region (8 KB for Settings). Instead `mount()` reads the 16-byte headers, then verifies full records from the highest sequence downwards and stops at the first good one. This matches the read-time estimate in `storage.md` §3 (8 headers, then one 1 KB record) and gives the same result.
7. **`load()` re-reads the record** instead of reusing what `mount()` left in the scratch buffer, because the firmware shares one scratch buffer between several Stores. This costs a second read of the record at boot (about 25 ms for the 1 KB settings record at 400 kHz). It also catches bit rot between mount and load (`Status::Corrupt`).
8. **Only `16 + length` bytes are written per save**, not the whole slot. `storage.md` §4 quotes 16 page writes for a 1 KB slot, which is the worst case (a full payload); typical records are shorter and cost fewer page cycles.
9. **Requirements:** `slot_count >= 2` (with one slot the newest record would be overwritten), `slot_size > 16`, `slot_size - 16 <= 65535`, and the region must fit in the 32-bit address space. Otherwise `mount()` returns `BadArg`. `scratch_len` limits the payload (`min(slot_size, scratch_len) - 16`); a record already on the device that is bigger than the scratch makes `mount()` return `TooLarge` rather than silently ignoring it.
10. **Failed saves do not advance the Store.** After an `IoError` the in-memory state is unchanged, so a retry targets the same slot again (the extra write cycle lands on that one slot; wear tolerance in the test accounts for it). After a real reboot, `mount()` decides from the memory contents: normally the old record, but if the cut left a complete valid record (the unwritten tail already held the right bytes) the new one.
11. **Wrap-safe comparison** is `int32_t(a - b) < 0`. It assumes live sequence numbers differ by less than 2³¹, which holds for any ring (they all lie within `slot_count` of the newest) unless a region is reused with stale slots from a different era. Equal sequences (never produced by `save`) are ordered by slot index so `mount()` is deterministic.
12. **Optional read-back verify** is off by default (`set_verify_after_write`). It compares in 32-byte chunks on the stack.

## `LogRing`

13. **The ring owns the first 4 bytes of each entry** (a little-endian sequence number). The design's fault-log entry lists `seq` first, so this fits it, but the application does not choose that value: `append` overwrites it. `0xFFFFFFFF` marks a blank entry, so the counter skips it when it wraps. `entry_size` is limited to 4..128 bytes because `append` builds the stamped entry in a stack buffer (no heap, no extra scratch argument, and a single `write` per entry, so one page write cycle).
14. **Entries have no CRC** (the fixed 16-byte fault-log layout has no room, and the design lists none). A power cut inside a page write cannot happen on real I²C EEPROMs (the page is programmed only after the STOP), but the byte-granular fake can cut inside an entry. Tests cover this: the ring always mounts, a cut before any byte leaves the old state, and a cut after the 4-byte sequence number leaves the new entry as the newest with the older entries intact (its tail may be torn). A cut inside the first 4 bytes is outside the guarantee (the sequence number itself would be torn), which is why entries should not straddle pages.
15. **`mount()` copes with an interrupted `clear()`.** `clear()` erases from the last entry down to entry 0, so blanks form a suffix. `mount()` finds the first blank, then the newest entry within the non-blank prefix by comparing to entry 0, and counts only the run up to the newest when blanks are present. Anything after it is stale and gets overwritten by later appends. A full ring has no blanks, so this adds one read (the last entry) and a binary search only when it is not full.
16. `read()` after `clear()` or on an empty ring returns `NotFound`.

## Open points for the firmware side

- The firmware's `At24Storage` must write in ascending order, split at 64-byte pages, and wait for the write cycle before returning.
- Choose the region magic per `Store` (Settings, Calibration, Counters) so one region never accepts another's records.
- The fault-log entry's `seq` field is filled by the ring; the firmware's own `seq` handling should be removed.
