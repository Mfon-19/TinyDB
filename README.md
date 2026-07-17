# TinyDB

<p align="center">
  <img src="assets/tinydb-mascot-clean.png" alt="TinyDB pixel mascot" width="420">
</p>

TinyDB is a C++23 embedded, ordered key-value database. It provides atomic
multi-key transactions, stable read snapshots, streaming cursors, synchronous
crash durability, automatic recovery, and values up to 4 MiB without running a
database server.

The permanent boundary is intentionally narrow: one process owns a database,
many readers may run concurrently, and one writer prepares at a time. The
application defines its own key schema and updates primary records and
secondary-index keys in the same transaction.

```cpp
#include <tinydb/database.h>

auto database = tinydb::Database::Open("notes.db").value();

auto write = database.BeginWrite().value();
write.Put("doc/42", "storage engine notes");
write.Put("tag/database/42", "");
auto commit = std::move(write).Commit().value();

auto read = database.BeginRead().value();
auto cursor = read.Scan(tinydb::KeyRange::Prefix("tag/database/")).value();
while (cursor.Valid()) {
  auto key = cursor.Key();       // borrowed until cursor movement
  auto value = cursor.CopyValue();
  cursor.Next();
}
```

## Guarantees

- Keys are unique byte strings ordered by unsigned lexicographic order.
- A write transaction publishes all of its puts and deletes, or none of them.
- A successful commit is WAL-durable before it becomes visible and before the
  call returns.
- A read transaction and its cursors retain one stable committed snapshot.
- Existing readers may delay writer publication; new readers cannot starve a
  publication that is ready to proceed.
- Recovery accepts only complete checksummed WAL transactions and is safe to
  repeat after interruption.
- Database pages, superblocks, and WAL records use fixed little-endian codecs,
  checksums, versions, and one database UUID.
- Persistent corruption and environmental failures are returned as statuses;
  they do not terminate the embedding process.
- An OS lock rejects a second process while the database is open.

A commit whose final synchronization reports an ambiguous failure returns
`IndeterminateCommit`. The handle then requires recovery; the application must
reopen and inspect an idempotency key written in the transaction to determine
its outcome.

TinyDB does not provide SQL, networking, replication, concurrent writers,
multi-process readers, custom comparators, or cross-database transactions. The
complete contract and protocol are in [doc/design.md](doc/design.md), with its
test evidence in [doc/guarantees.md](doc/guarantees.md).

## Architecture

```text
Database / transactions / cursors
              |
        B+ tree and allocator
              |
 write overlay | committed immutable cache
              |             |
       commit coordinator    | checkpoint
              |             |
           WAL segments    database file
                 \         /
                   recovery
```

A writer copies pages into a private transaction overlay. Commit freezes and
validates the final page images, reserves everything publication needs, appends
one self-binding physical WAL transaction, and synchronizes it. Only then does
it wait for older readers and transfer the prepared page buffers into the
committed cache. Publication performs no allocation or I/O.

The database file contains the latest checkpoint. WAL segments contain newer
durable commits. A checkpoint writes exact captured cache versions, syncs those
pages, advances the inactive superblock, and only then removes covered WAL
segments. Recovery physically replays complete transactions; it never reruns
tree mutations.

## Build and test

The default build needs CMake 3.25 and a C++23 compiler:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The focused CTest suite contains 60 cases: 59 discovered cases across seven
test binaries plus one installed-package consumer. It covers the public
contract, model transactions, pages and persistent codecs, durability ordering,
concurrency, fault injection, and deterministic process-kill sweeps.

CI runs GCC and Clang in Debug and Release configurations, ASan+UBSan, TSan,
the crash sweeps, and external format golden files.
The same sanitizer configurations are available locally:

```sh
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan -LE crash
```

## Benchmark

The benchmark measures TinyDB alone. Setup and warmup are excluded from timed
trials; fixed seeds make access order reproducible, and repeated trials report
mean, sample deviation, minimum, p50, p95, p99, and maximum. Every timed
workload validates its results.

```sh
cmake -S . -B build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DTINYDB_BUILD_BENCHMARKS=ON
cmake --build build/bench --target TinyDB_bench
build/bench/TinyDB_bench --rows 5000 --trials 7 --warmups 2
```

CSV output covers insert and overwrite commits, convenience and
transaction-scoped hot point reads, cursor scans, checkpoints, process-restart
recovery, and space reuse under churn. Use `--workload` to isolate one family
and `--minimum-trial-ms` to lengthen CPU-bound samples. The harness records
measurements; this repository does not embed machine-specific claims.

## Repository

| Path | Purpose |
|---|---|
| `include/tinydb` | Public application API only |
| `src/api` | Handle ownership and public operations |
| `src/txn` | Transaction overlay, commit protocol, reader gate |
| `src/btree` | Ordered-map algorithms, page views, overflow values |
| `src/cache` | Immutable committed-page cache |
| `src/storage` | Page/superblock codecs and database file |
| `src/wal`, `src/recovery`, `src/checkpoint` | Durability lifecycle |
| `src/verify` | Read-only structural verification |
| `tests`, `bench` | Guarantee tests and reproducible measurements |
| `tools` | Binary-safe dump utility |
