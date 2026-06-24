# tests — Test suite

Where correctness is enforced. A storage engine is pointer- and byte-layout-heavy
and must survive crashes, so testing is first-class — and for this project the
testing *is* the impressive part. Four kinds of tests live here.

## 1. Unit tests (per layer)
One file per component, mirroring `src/`:
- `disk_manager_test.cpp` — page read/write round-trips, durable sync, freelist
  reuse.
- `buffer_pool_test.cpp` — fetch/pin/unpin, eviction, dirty flushing.
- `codec_test.cpp` — slice, varint, and cell encode/decode round-trips.
- `btree_test.cpp` — insert/search/delete, node splits, range scans.
- `wal_test.cpp` — append, flush, iterate, checksum/torn-tail detection.

## 2. Differential (model-based) tests — the correctness backbone
`differential/` — run the **same random sequence** of `put`/`get`/`remove`/`scan`
against the engine and against a reference `std::map<string,string>`, asserting
they always agree. This is how real engines find B+-tree bugs; the randomized,
long-running version is the highest-value bug finder in the suite.

## 3. Crash / chaos tests — the headline
`chaos/` — the signal that separates this from a toy:
```
loop thousands of times:
  apply random put/delete operations
  kill the process at a random point (no clean close)
  reopen → recovery runs
  verify contents match the expected committed state
```
Validates `src/wal` + `src/recovery`. Implemented with process-level kills and/or
a test `DiskManager`/fault-injection wrapper that simulates torn writes,
partial writes, and failed syncs.

## 4. Persistence / reopen tests
`persistence/` — write data, close cleanly, reopen, and assert everything is
still there and correctly ordered.

## Tooling
- Framework: GoogleTest.
- Debug builds run under **ASan/UBSan**; differential + chaos tests are the
  highest-value bug finders. The codec decoders are also a **libFuzzer** target.
