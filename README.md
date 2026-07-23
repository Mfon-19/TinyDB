# TinyDB

<p align="center">
  <img src="assets/tinydb-mascot-clean.png" alt="TinyDB pixel mascot" width="420">
</p>

TinyDB is a C++ storage engine for applications that need a durable, ordered map without operating a database server. It provides atomic multi-key transactions, snapshot reads, streaming range scans, and crash recovery through an embedded API, while leaving schemas and indexes under application control.

## Quick start

The application owns its schema. A document and its application-defined index
keys can be changed in one transaction and later traversed by prefix:

```cpp
#include <tinydb/database.h>

#include <iostream>
#include <stdexcept>
#include <utility>

template <class T>
auto Must(tinydb::Result<T> result) -> T {
  if (!result) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

void Must(tinydb::Status status) {
  if (!status.Ok()) {
    throw std::runtime_error(status.ToString());
  }
}

auto main() -> int {
  auto database = Must(tinydb::Database::Open("notes.db"));

  auto write = Must(database.BeginWrite());
  Must(write.Put("doc/42", "storage engine notes"));
  Must(write.Put("tag/database/42", ""));
  (void)Must(std::move(write).Commit());

  {
    auto read = Must(database.BeginRead());
    auto cursor = Must(read.Scan(tinydb::KeyRange::Prefix("tag/database/")));
    while (cursor.Valid()) {
      std::cout << cursor.Key() << '\n';
      Must(cursor.Next());
    }
  }

  Must(database.Close());
}
```

`Put` replaces an existing value, `Delete` succeeds when a key is absent, and
ranges use half-open bounds: `[lower, upper)`.

## Application contract

| Property | Contract |
|---|---|
| Data model | Unique byte-string keys and byte-string values; empty keys and values are valid |
| Ordering | Unsigned lexicographic byte order, stable across supported platforms |
| Transactions | Atomic multi-key writes with read-your-writes behavior |
| Reads | Stable transaction snapshots and forward streaming cursors |
| Isolation | Strictly serializable within the owning process |
| Concurrency | Many read transactions and at most one write transaction |
| Durability | A successful commit survives process and system crashes, subject to synchronization guarantees from the filesystem and device |
| Recovery | `Open()` automatically restores the latest complete durable transaction |
| Ownership | One process holds an exclusive lock on the database |
| Corruption | Detected persistent damage is reported and is never silently repaired |

TinyDB uses explicit little-endian encodings, checksums, format versions, and a
database UUID. A binary rejects unsupported persistent formats rather than
reinterpreting their bytes.

## Failure semantics

The return value of a storage operation tells the application what it may
believe:

- A successful `Commit()` is both visible and durable. It does not depend on a
  later `Checkpoint()` or `Close()`.
- A failure before the durability point aborts the transaction without
  changing committed state.
- `IndeterminateCommit` means the final synchronization may have crossed the
  durability boundary. The handle enters `NeedsRecovery`; reopen the database
  and inspect an application idempotency key to determine the outcome.
- I/O failures, resource exhaustion, unsupported formats, and detected
  corruption cross the API as `Status` values.
- `Checkpoint()` advances the self-contained database file and shortens future
  recovery. `Close()` releases resources and may request a final checkpoint;
  neither is the mutation durability boundary.

## Why TinyDB

TinyDB is intended for local application state that naturally fits an ordered
map: desktop and command-line tools, durable metadata, local indexes, caches,
and per-node state inside a larger system. Embedding removes the network hop
and the operational cost of deploying a separate database service.

The application remains responsible for its key encoding and logical schema.
Secondary indexes are ordinary keys updated atomically with their primary
record:

```text
doc/42                         -> encoded document
tag/database/42                -> empty
updated/2026-07-17T10:00/42    -> empty
```

## Architecture

`Database` owns the process lock and every storage subsystem. A read
transaction registers one visible root and keeps that snapshot stable until
its transaction and cursors are gone. Its B+ tree lookups read immutable pages
from the committed cache, which loads checkpointed pages from the database file
on demand.

One write transaction may exist at a time. B+ tree mutations copy pages into a
private overlay; reads consult private versions first and fall through to the
committed cache for unchanged pages. The resulting root and allocator state
are private as well, so aborting only discards the overlay.

Commit freezes and validates the finished overlay, appends its final page
images and database state to the WAL, writes a record binding them into one
transaction, and synchronizes the WAL. Only after that durability point does
the writer wait for older readers and publish the prepared pages into the
committed cache. Publication performs no file I/O or allocation, so a durable
transaction cannot fail halfway through becoming visible.

The database file contains state through the latest checkpoint. WAL segments
contain committed transactions after it. A checkpoint writes exact committed
page versions, synchronizes the database file, advances its checksummed
superblock, and only then removes covered WAL segments. `Open()` selects the
newest valid superblock and physically replays complete WAL transactions;
recovery never reruns `Put`, `Delete`, splitting, or allocation logic.

## Public API

| Type | Role |
|---|---|
| `Database` | Owns the process lock, storage subsystems, convenience operations, checkpoints, verification, and statistics |
| `ReadTransaction` | Retains one stable committed snapshot |
| `WriteTransaction` | Owns one private mutation overlay and commits or aborts it atomically |
| `Cursor` | Streams an ordered range without materializing the result |
| `KeyRange` | Describes all, prefix, lower-bounded, upper-bounded, or half-open scans |
| `Status` / `Result<T>` | Carries application, environmental, format, and corruption failures |

Convenience `Database::Get`, `Put`, and `Delete` operations use the same
transaction paths as the explicit API.

## Limits and non-goals

| Limit | Value |
|---|---:|
| Maximum key | 1,024 bytes |
| Maximum value | 4 MiB |
| Persistent page size | 4,096 bytes |
| Default committed-page cache target | 16 MiB, configurable |
| Default write-transaction memory limit | 16 MiB, configurable |
| Concurrent owning processes | 1 |
| Concurrent write transactions | 1 |

TinyDB intentionally does not provide:

- SQL, schemas, joins, or engine-maintained secondary indexes
- networking, authentication, or a server protocol
- replication, high availability, or distributed transactions
- multiple processes opening one database, including read-only processes
- concurrent writers or cross-database transactions
- custom key comparators

## Build and test

TinyDB requires CMake 3.25 and a C++23 compiler:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The focused test suite covers the public contract, model transactions,
persistent codecs, durability ordering, concurrency, fault injection, and
deterministic process-kill recovery sweeps. Sanitizer configurations are also
available locally:

```sh
cmake --preset asan
cmake --build --preset asan
ctest --preset asan

cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan -LE crash
```

## Command-line tools

The `tinydb` executable is a one-command-per-process front end to the public
API:

```text
tinydb <database> put <key> <value>
tinydb <database> get <key>
tinydb <database> del <key>
tinydb <database> scan
tinydb <database> scan <lower> <upper>
```

Bounded scans use `[lower, upper)`. Unbounded scans stream the entire database.
The CLI is intended for basic inspection and scripting, not as an interactive
shell.

`tinydb_dump` performs recovery and a full integrity verification before
writing every key and value as reversible hexadecimal:

```text
tinydb_dump <database>
```

## Benchmarking

The benchmark excludes fixture construction from timed regions, discards
warmups, uses deterministic access order, validates
every workload, and summarizes repeated samples with mean, sample deviation,
minimum, p50, p95, p99, and maximum.

```sh
cmake -S . -B build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DTINYDB_BUILD_BENCHMARKS=ON
cmake --build build/bench --target TinyDB_bench
build/bench/TinyDB_bench
```

The CSV workloads cover insert and overwrite commits, hot point reads,
metadata-only and value-copy cursor scans, checkpoints, process-restart
recovery, and space reuse under churn. Run one family or lengthen CPU-bound
trials with:

```sh
build/bench/TinyDB_bench --workload reads --minimum-trial-ms 1000
```

### Reference results

The following results are from a complete run. They
describe this machine and build; they are a reproducible reference, not a
performance guarantee for other systems.

| Component | Configuration |
|---|---|
| CPU | Intel Core i5-1135G7, 4 cores / 8 threads, 2.4 GHz base and 4.2 GHz maximum |
| Memory | 7.5 GiB |
| Storage | Intel SSDPEKNW512G8 NVMe SSD; ext4 on an LVM logical volume |
| Operating system | Linux 6.17.0-29-generic, x86-64 |
| Compiler | GCC 13.3.0, Release build |

The run used 5,000 rows, 128-byte values, a 16 MiB cache, 64 transactions of
16 writes each, seven measured trials after two warmups, a 250 ms minimum for
CPU-bound trials, and deterministic seed `92673823818818`. Put throughput
counts individual key updates; the corresponding transaction rate is shown in
parentheses.

| Workload | Median result | Tail and storage details |
|---|---:|---|
| Insert new keys | 6,519 updates/s (407 transactions/s) | Commit p50 1.607 ms, p95 2.182 ms, p99 2.580 ms; WAL amplification 3.95× |
| Overwrite keys | 7,202 updates/s (450 transactions/s) | Commit p50 1.500 ms, p95 1.946 ms, p99 2.301 ms; WAL amplification 4.09× |
| Hot `Database::Get` | 28,963 reads/s | 34.53 µs/read; 100% TinyDB cache-hit rate |
| Hot `ReadTransaction::Get` | 29,044 reads/s | 34.43 µs/read; 100% TinyDB cache-hit rate |
| Cursor metadata scan | 2.837 million rows/s | 352.5 ns/row |
| Cursor value-copy scan | 2.690 million rows/s | 371.8 ns/row |
| Checkpoint | 3.208 ms | 235.0 MiB/s dirty-page transfer; p95 3.639 ms |
| Process-restart recovery | 22.346 ms | 42.41 MiB/s replay; p95 23.741 ms |
| Three-round churn | 22,539 operations/s | 1.107× file amplification; 819,200-byte final database file |

Process-restart recovery is filesystem-cache-warm: the writer exits without a
TinyDB close, but the harness does not evict the operating system's page cache.
The insert run also recorded one 16.276 ms commit outlier; its p99 remained
2.580 ms. The machine was not otherwise isolated from normal operating-system
activity.

## Repository

| Path | Purpose |
|---|---|
| `include/tinydb` | Public application API |
| `src/api` | Public handles and subsystem ownership |
| `src/txn` | Transaction overlay, reader gate, and commit protocol |
| `src/btree` | Ordered-map algorithms, page views, and overflow values |
| `src/cache` | Immutable committed-page cache |
| `src/storage` | Database file, superblocks, and persistent page codecs |
| `src/io` | POSIX I/O, locking, and injectable syscall boundary |
| `src/wal` | Segmented write-ahead log |
| `src/recovery` | WAL validation and physical redo |
| `src/checkpoint` | Durable database-file advancement |
| `src/verify` | Read-only structural integrity verification |
| `src/cli` | One-shot command-line interface |
| `tests` | Guarantee-oriented tests and format fixtures |
| `bench` | Repeatable TinyDB measurement harness |
| `tools` | Binary-safe inspection utilities |
