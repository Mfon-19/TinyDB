# TinyDB

<p align="center">
  <img src="assets/tinydb-mascot-clean.png" alt="TinyDB pixel mascot" width="420">
</p>

TinyDB is a small, embeddable key-value storage engine. It is meant to run
inside an application's process as a layer for durable storage.

## Build and run

Requires Linux, CMake 3.24 or newer, and a C++23 compiler and standard library
with support for `std::expected` and `std::format`.

```sh
cmake -S . -B build -DTINYDB_BUILD_TESTS=OFF
cmake --build build -j
./build/tinydb_cli example.db
```

This is a small repl for writing and reading to the database

```text
put greeting "hello world"
get greeting
scan
delete greeting
checkpoint
quit
```

Use double quotes for spaces or empty strings. Run `help` to list the commands.

## Usage

The preferred way to read or write to TinyDB is through transactions. This
example opens a database with a buffer pool of 64 pages:

```cpp
#include "tinydb/database.h"
#include <iostream>

int main() {
  auto database = tinydb::Database::Open("example.db", 64).value();
  auto writer = database->BeginWrite().value();
  if (auto status = writer->Put("greeting", "hello world"); !status.Ok()) {
    std::cerr << status.Message() << '\n';
    return 1;
  }
  if (auto status = writer->Commit(); !status.Ok()) {
    std::cerr << status.Message() << '\n';
    return 1;
  }

  auto reader = database->BeginRead().value();
  auto value = reader->Get("greeting").value();
  if (value) {
    std::cout << *value << '\n';
  }
}
```

Save this as `example.cpp`, then compile and run it from the repository root:

```sh
c++ -std=c++23 -pthread -Iinclude example.cpp build/libtinydb.a -o example
./example
```

The example uses `.value()` for brevity; it throws if a `Result` contains an
error. Callers can instead check the result and read `result.error().Message()`.
A successful `Get` returns an empty optional when the key is absent.

For a complete runnable example, see [examples/example.cpp](examples/example.cpp).
It demonstrates batched writes, reading pending changes, rollback, cursor scans,
deletion, checkpointing, and reopening, with error handling. It writes sample
`user:` keys to the database path you supply:

```sh
cmake -S . -B build -DTINYDB_BUILD_TESTS=OFF
cmake --build build --target tinydb_example
./build/tinydb_example example.db
```

## How it works

TinyDB features a disk manager that talks to the operating system (Linux) to
read and write to files, a buffer pool that holds 4 KiB pages in in-memory
frames, a B+ tree over a page context backed by either transaction-local pages
or the buffer pool, and read/write transactions enabling a single writer and
multiple readers.

For a write transaction, every write goes to pages in a transaction-local area,
enabling read-your-writes consistency. On commit, dirty pages are written
to the WAL (write-ahead log), the WAL is flushed to disk, and only then are the
dirty pages installed in the buffer pool and made available to readers. If the
commit triggers a checkpoint, the pages are written directly to the database
file instead. After a checkpoint, the WAL is reused from the start rather than
truncated; a random salt in its header marks which records are current.

For a read transaction, readers acquire a shared visibility lock and proceed
to read. Writers wait for these readers to finish before publishing committed
changes.

## Benchmarks

Benchmarked on my machine: Intel Core i5-1135G7 with four online CPUs
(Hyper-Threading disabled),
7.5 GiB RAM, and an Intel SSDPEKNW512G8 NVMe drive running ext4 over LVM.
The build used GCC 13.3.0, `-O3 -DNDEBUG`, Release link-time optimization,
and Linux 7.0.0-31-generic.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DTINYDB_BUILD_TESTS=OFF
cmake --build build-release --target tinydb_bench -j
./build-release/tinydb_bench ./build-release/bench-data all --keys 10000 --runs 3
```

The benchmark uses 10,000 16-byte keys, 100-byte values, a 256-page pool (1 MiB),
100 writes per transaction, and seed 42. Throughput and per-run p99 latencies
are medians of three runs; parentheses show the throughput range. Reads use
a warm Linux file cache. Scan throughput counts entries per second.
The mixed workload runs four reader threads alongside one writer; readers keep
reading until the writer finishes.

| Workload | Operations/s, median (min–max) | p99 transaction (ms) |
| --- | ---: | ---: |
| Sequential inserts | 129,220 (129,163–136,279) | 2.81148 |
| Random inserts | 43,532 (41,246–44,446) | 4.79213 |
| Existing-key reads | 1,261,375 (1,039,726–1,277,783) | 0.00282 |
| Missing-key reads | 1,286,540 (1,176,257–1,300,473) | 0.00262 |
| 100-entry scans | 24,505,540 (11,881,056–28,311,208) | 0.01566 |
| Full scans | 14,939,800 (8,113,136–16,139,731) | — |
| Overwrites | 46,140 (46,115–47,907) | 3.93407 |
| Deletes | 46,972 (46,781–47,393) | 3.57150 |
| Reinserts | 49,406 (49,153–49,431) | 3.43164 |
| Reads with one active writer | 611,891 (581,378–626,686) | 0.02586 |
| Writes with four active readers | 33,994 (32,984–36,336) | 4.48864 |

### Comparing with SQLite

TinyDB had higher throughput in every workload. Its lead was largest for
point reads and scans; for writes it was 1.06–1.18×. TinyDB used 8.4% less
disk space after sequential inserts, while SQLite used less after random
inserts.

These results compare TinyDB with SQLite 3.45.1 on the machine described above,
with Release link-time optimization enabled for TinyDB.
Both engines used the same 10,000 16-byte keys, 100-byte values, seed 42,
100 writes per transaction, and a 256-page (1 MiB) cache budget. SQLite used a
`WITHOUT ROWID` table with BLOB keys and values, reused prepared statements,
WAL mode with `synchronous=FULL`, 4 KiB pages, and no memory mapping. Its
cache budget is advisory; neither engine's budget limits the OS file cache.
SQLite's automatic checkpoint threshold was 128 WAL frames, comparable to
TinyDB's half-pool threshold, though their checkpoint implementations differ.

The table shows median throughput across three runs. Engine order alternated
between runs, reads used a warm OS file cache, and all correctness checks
passed. Each run measured one million lookups per read workload, 100,000
100-entry scans, and 1,000 full scans. This comparison is single-threaded;
the TinyDB-only table above uses fewer read and scan passes in a separate run.

| Workload | TinyDB | SQLite | Faster |
| --- | ---: | ---: | --- |
| Sequential inserts | 130,573 ops/s | 119,654 ops/s | TinyDB 1.09× |
| Random inserts | 45,750 ops/s | 38,701 ops/s | TinyDB 1.18× |
| Existing-key reads | 1,271,067 ops/s | 529,973 ops/s | TinyDB 2.40× |
| Missing-key reads | 1,361,622 ops/s | 559,091 ops/s | TinyDB 2.44× |
| 100-entry scans | 480,066 scans/s | 68,674 scans/s | TinyDB 6.99× |
| Full scans | 11,621,224 entries/s | 7,194,264 entries/s | TinyDB 1.62× |
| Overwrites | 47,596 ops/s | 43,318 ops/s | TinyDB 1.10× |
| Deletes | 45,356 ops/s | 42,902 ops/s | TinyDB 1.06× |
| Reinserts | 48,193 ops/s | 41,364 ops/s | TinyDB 1.17× |

TinyDB's narrower API may explain its lead on point reads and short scans:
operations call the B+ tree directly, and cursors read views into leaf pages.
SQLite still executes [bytecode](https://www.sqlite.org/opcode.html) for
prepared statements. Avoiding that execution layer can matter for small,
cached operations, though this benchmark does not isolate its contribution.
Short scans also benefit from dense leaves after sequential loading: the
benchmark repeats the same 100 starting points, whose pages fit in TinyDB's
cache. The scan advantage depends on this access pattern.

To reproduce this comparison, install SQLite's development package
(`libsqlite3-dev` on Ubuntu) and run:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DTINYDB_BUILD_TESTS=OFF -DTINYDB_BENCH_SQLITE=ON
cmake --build build-release --target tinydb_bench -j
./build-release/tinydb_bench ./build-release/bench-data all \
  --engine both --keys 10000 --batch 100 --runs 3 \
  --read-passes 100 --scan-passes 1000 > build-release/comparison.csv
python3 benchmarks/compare.py build-release/comparison.csv
```
