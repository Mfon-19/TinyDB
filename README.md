# TinyDB

A disk-backed, ordered **key-value storage engine** in C++20. A page-based
file format, a buffer pool, and a B+-tree index behind a small embedded API.

It is the persistence core that sits *underneath* a database, not a full
RDBMS: no SQL, planner, or server, just storage, indexing, caching, and
durability.

```cpp
auto db = tinydb::StorageEngine::Open("data.db");

db.Put("user:1", "Mfon");
db.Get("user:1");             // -> std::optional{"Mfon"}
db.Remove("user:1");

for (const auto &[key, value] : db.Scan("user:", "user;"))  // [start, end)
  std::cout << key << " = " << value << "\n";
```

## What's inside

- **4 KiB slotted pages** over plain `pread`/`pwrite`, with an intrusive
  LIFO free list so deleted pages are reused instead of growing the file.
- **Buffer pool** — fixed-frame page cache with pin counts, dirty tracking,
  and clock eviction.
- **B+ tree** — variable-length keys and values, splits (tail-heavy for
  ascending inserts), merges, and leaf-chained range scans.
- **Durability at close** — `Close()` flushes and fsyncs.
- Invariant checks (`TINYDB_CHECK`) stay on in release builds: corruption
  aborts loudly rather than propagating.

## Build, test, benchmark

```sh
cmake -B build && cmake --build build      # library
ctest --test-dir build                     # unit tests

cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release -DTINYDB_BUILD_BENCHMARKS=ON
cmake --build cmake-build-release --target TinyDB_workloads_bench
./cmake-build-release/TinyDB_workloads_bench
```

Sample numbers (NVMe SSD, 112-byte entries): ~420k sequential inserts/s,
~264k random inserts/s, point reads at 1.9 µs from the buffer pool, 4.1 µs
from the OS page cache, and 110 µs from the device; range scans at
~690 MiB/s. See `bench/workloads_bench.cpp` for methodology and caveats.

## Layout

| Path | What's there |
|------|--------------|
| `include/tinydb` | Public headers (the embed surface) |
| `src/storage` | DiskManager: page file, header, free list |
| `src/buffer`  | Buffer pool / page cache |
| `src/btree`   | B+ tree and the on-disk node formats |
| `src/engine`  | StorageEngine API and lifecycle |
| `src/cli`     | Interactive REPL |
| `tests`       | GoogleTest suites for every layer |
| `bench`       | Google Benchmark workload suite |

## What's next

- **Write-ahead log** — per-operation durability instead of
  durability-at-close, into `src/wal`.
- **Crash recovery** — replay the log on `Open()` after an unclean shutdown,
  into `src/recovery`; then checkpointing to bound replay time.
- **Recovery and commit-latency benchmarks** — fsync cost per commit, time to
  recover after a crash.
- **Engine metrics** — buffer-pool hit rate, write amplification, leaf fill
  factor, and latency percentiles in the benchmark suite.
- **Open-ended scans** — `Scan(start)` to the end of the keyspace, without a
  sentinel end key.
- **And a few more...**