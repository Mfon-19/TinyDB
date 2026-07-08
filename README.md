# TinyDB

<p align="center">
  <img src="assets/tinydb-mascot.png" alt="TinyDB pixel mascot" width="420">
</p>

A disk-backed, ordered **key-value storage engine** in C++23. A page-based
file format, a buffer pool, and a B+-tree index behind a small embedded API.

It is the persistence core that sits *underneath* a database, not a full
RDBMS: no SQL, planner, or server, just storage, indexing, caching, and
durability.

```cpp
auto db = tinydb::StorageEngine::Open("data.db").value();

db.Put("user:1", "Mfon");     // -> tinydb::Status
db.Get("user:1");             // -> Result<std::optional<std::string>>
db.Remove("user:1");

for (const auto &[key, value] : db.Scan("user:", "user;").value())  // [start, end)
  std::cout << key << " = " << value << "\n";
```

## Architecture

Four layers — here drawn as the structures they are. One key's journey,
top to bottom:

```text
                   Put · Get · Remove · Scan
                               │
                     ╔═════════▼═════════╗
                     ║   StorageEngine   ║      src/engine — the one door
                     ╚═════════╤═════════╝      in; every failure walks
                               │                back out of it as a
                               │                Status / Result<T>
                          [ B+ tree ]           src/btree
                            ┌─────┐
                            │ k37 │             internal nodes route
                            └┬───┬┘             the key down
                       ┌─────┘   └─────┐
                   ┌───▼───┐       ┌───▼───┐
              ┄┄──▶│ k1 k9 │──────▶│k37 k80│──▶┄┄
                   └───────┘       └───────┘    leaves hold the rows,
                               │                chained for Scan
                               │  every node is one 4 KiB page,
                               ▼  fetched and pinned via PageRef
                        [ Buffer pool ]         src/buffer
                ┌────┬────┬────┬────┬────┬────┬────┐
                │ p3*│p17 │ p9*│ …  │p41 │p88*│ p5 │  64 fixed frames
                └────┴────┴────┴────┴────┴────┴────┘  * = dirty
                 pins hold pages in; a clock hand
                 sweeps for the eviction victim
                               │
                        [ DiskManager ]         src/storage
                               │
                               │  pread · pwrite · fsync
                               ▼
             ┌────────┬────────┬────────┬────────┬┄┄
             │ header │ btree  │  free  │ btree  │┄┄  data.db
             └───┬────┴────────┴───▲────┴────────┴┄┄
                 └─ free-list head ┘
                 freed pages are reused before the file grows
```

Each layer only talks to the one directly below it: the tree never sees the
file, the pool never parses a node. The write-ahead log (next up) will slot
in beside the buffer pool, getting its records to disk before the pages
they describe.

## What's inside

- **4 KiB slotted pages** over plain `pread`/`pwrite`, with an intrusive
  LIFO free list so deleted pages are reused instead of growing the file.
- **Buffer pool** — fixed-frame page cache with pin counts, dirty tracking,
  and clock eviction.
- **B+ tree** — variable-length keys and values, splits (tail-heavy for
  ascending inserts), merges, and leaf-chained range scans.
- **Durability at close** — `Close()` flushes and fsyncs.
- **Status-based errors** — I/O failures travel as `Status` / `Result<T>`
  (`std::expected`) values, LevelDB-style; nothing throws.
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
