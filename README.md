# TinyDB

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
file instead.

For a read transaction, readers acquire a shared visibility lock and proceed
to read. Writers wait for these readers to finish before publishing committed
changes.

## Benchmarks

Benchmarked on my machine: Intel Core i5-1135G7 with four online CPUs,
7.5 GiB RAM, and an Intel SSDPEKNW512G8 NVMe drive running ext4 over LVM.
The build used GCC 13.3.0, `-O3 -DNDEBUG`, and Linux 6.17.0-29-generic.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DTINYDB_BUILD_TESTS=OFF
cmake --build build-release --target tinydb_bench -j
./build-release/tinydb_bench ./build-release/bench-data all --keys 10000 --runs 3
```

The baseline uses 10,000 16-byte keys, 100-byte values, a 256-page pool (1 MiB),
100 writes per transaction, and seed 42. Throughput and per-run p99 latencies
are medians of three runs; parentheses show the throughput range. Reads use
a warm Linux file cache.

| Workload | Operations/s, median (min–max) | p99 transaction (ms) |
| --- | ---: | ---: |
| Sequential inserts | 8,565 (8,311–8,566) | 15.666 |
| Random inserts | 6,767 (6,468–6,809) | 18.977 |
| Existing-key reads | 12,634 (12,572–12,679) | 0.089 |
| Missing-key reads | 12,687 (12,257–12,718) | 0.090 |
| 100-entry scans | 147,046 (142,027–147,608) | 0.762 |
| Full scans | 164,126 (155,452–165,174) | — |
| Overwrites | 6,054 (6,009–6,142) | 19.484 |
| Deletes | 3,612 (3,558–3,633) | 30.771 |
| Reinserts | 5,909 (5,533–5,918) | 23.232 |