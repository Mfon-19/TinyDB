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
| Default committed-page cache target | 256 KiB, configurable |
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

The benchmark suite covers writes, reads, scans, mixed traffic, reader/writer
concurrency, checkpoints, process-restart recovery, churn, large values, and
working-set scaling. Named `smoke`, `standard`, and `soak` profiles keep the
scenario geometry reproducible; `standard` is the default performance profile.

```sh
cmake -S . -B build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DTINYDB_BUILD_BENCHMARKS=ON
cmake --build build/bench --target TinyDB_bench
build/bench/TinyDB_bench --profile standard
```

Each run writes raw observations, statistical summaries, and machine/build
metadata under `benchmark-results/`. Scenario order is randomized from a
recorded seed. Fixture construction and warmups are outside timed regions, and
every workload validates its results. List or select scenarios with:

```sh
build/bench/TinyDB_bench --profile standard --list
build/bench/TinyDB_bench --profile standard --family reads
build/bench/TinyDB_bench --profile standard --filter read.eviction.hotspot
```

The [benchmark guide](bench/README.md) defines the profiles, cache-state terms,
artifact formats, host preparation, and the alternating baseline/candidate
runner.

### Reference smoke results: short validation

The following results are from a complete smoke-profile run on July 17, 2026.
They verify the suite end to end and provide a machine-specific sanity
reference; use the standard profile for optimization or release claims.

| Component | Configuration |
|---|---|
| CPU | Intel Core i5-1135G7, 4 cores / 8 threads, 2.4 GHz base and 4.2 GHz maximum |
| Memory | 7.5 GiB |
| Storage | Intel SSDPEKNW512G8 NVMe SSD; ext4 on an LVM logical volume |
| Operating system | Linux 6.17.0-29-generic, x86-64 |
| Compiler | GCC 13.3.0, Release build |
| CPU policy | Linux `powersave` scaling governor; host otherwise unisolated |

The smoke profile used a 1 MiB cache, three measured trials after one warmup,
a 100 ms CPU-workload floor, eight commits per write trial, and deterministic
seed `92673823818818`. Fixtures vary by scenario; the run metadata records
every row count, key/value size, cache ratio, and execution order.

| Scenario | Median result | Tail and cache/storage details |
|---|---:|---|
| Sequential insert, batch 1 | 626 updates/s | Commit p50 1.515 ms, p95 2.036 ms, p99 2.309 ms; 29.89× WAL amplification |
| Random insert, batch 16 | 6,074 updates/s | Commit p50 1.878 ms, p95 2.247 ms, p99 2.278 ms; 7.94× WAL amplification |
| Random overwrite, batch 16 | 7,100 updates/s | Commit p50 1.685 ms, p95 2.147 ms, p99 2.182 ms; 10.43× WAL amplification |
| Engine-hot transaction reads | 38,142 reads/s | 26.22 µs/read; 100% TinyDB cache-hit rate |
| 8×-cache uniform reads | 22,820 reads/s | 43.82 µs/read; 78.50% TinyDB cache-hit rate |
| Full metadata scan | 167,365 rows/s | 5.975 µs/row; 0.15% TinyDB cache-hit rate |
| Full value-copy scan | 165,809 rows/s | 6.031 µs/row; 0.15% TinyDB cache-hit rate |
| Writer with four readers | 4,632 updates/s and 93,958 reads/s | Commit p50 2.588 ms, p95 2.733 ms, p99 3.066 ms |
| 2 MiB checkpoint | 5.601 ms | 461.0 MiB/s dirty-page transfer; p95 5.979 ms |
| 2 MiB OS-warm recovery | 56.453 ms | 47.84 MiB/s replay; p95 56.951 ms |
| Steady-state churn | 22,069 operations/s | 2.244× file amplification; zero measured file growth per round |

Recovery is OS-cache-warm: the writer exits without closing TinyDB, but the
harness does not evict the operating system or device cache. The host was not
isolated from normal operating-system activity.

### Reference soak results: long diagnostic

The following results are from a complete soak-profile run later on July 17,
2026. Unlike the smoke run above, this run exercised the largest fixture sizes,
with most scenarios using 30 measured trials after five warmups, 5-second
CPU-workload floors, and 3,840 commit-latency samples per write scenario. The
churn workload used five trials of 40 measured rounds. The run completed all 38
scenarios and collected 65,195 observations in 13 hours, 21 minutes.

Smoke and soak results are deliberately reported separately. The profiles use
different fixture geometry, cache sizes, trial lengths, and operation counts,
so differences between their raw rates do not represent a TinyDB performance
regression or improvement.

| Component | Configuration |
|---|---|
| CPU | Intel Core i5-1135G7, 4 cores / 8 threads, 2.4 GHz base and 4.2 GHz maximum |
| Memory | 7.5 GiB |
| Storage | Fixtures under `/tmp` on ext4 |
| Operating system | Linux 6.17.0-29-generic, x86-64 |
| Compiler | GCC 13.3.0, Release build |
| CPU policy | Linux `powersave` scaling governor; host otherwise unisolated |
| Source state | Base commit `e00f65d`; worktree included uncommitted benchmark-suite changes |

The soak profile used a 32 MiB TinyDB page-cache target and deterministic seed
`92673823818818`. Each scenario's row count and logical working-set size are
recorded separately in the run metadata.

| Scenario | Median result | Tail and cache/storage details |
|---|---:|---|
| Sequential insert, batch 1 | 564 updates/s | Commit p50 1.624 ms, p95 2.205 ms, p99 2.507 ms; 31.69× WAL amplification |
| Random insert, batch 16 | 5,576 updates/s | Commit p50 2.327 ms, p95 2.943 ms, p99 3.380 ms; 25.31× WAL amplification |
| Sequential insert, batch 256 | 22,229 updates/s | Commit p50 2.268 ms, p95 2.828 ms, p99 3.184 ms; 1.364× WAL amplification |
| Engine-hot transaction reads | 28,819 reads/s | 34.70 µs/read; 100% TinyDB cache-hit rate |
| 8×-cache uniform reads | 6,406 reads/s | 156.10 µs/read; 82.53% TinyDB cache-hit rate |
| 32×-cache uniform reads | 2,303 reads/s | 434.23 µs/read; 76.94% TinyDB cache-hit rate |
| Full metadata scan | 28,879 rows/s | 34.63 µs/row; effectively no TinyDB cache hits |
| Full value-copy scan | 28,338 rows/s | 35.29 µs/row; effectively no TinyDB cache hits |
| Uniform 80/12/4/4 mixed traffic | 4,038 operations/s | Commit p50 1.777 ms, p95 2.385 ms, p99 2.656 ms |
| Writer without readers | 3,904 updates/s | Commit p50 2.401 ms, p95 2.994 ms, p99 3.300 ms |
| Writer with four readers | 1,290 updates/s and 10,761 reads/s | Commit p50 5.271 ms, p95 7.050 ms, p99 8.015 ms |
| Writer with eight readers | 420 updates/s and 6,367 reads/s | Commit p50 15.65 ms, p95 28.04 ms, p99 33.30 ms |
| 64 MiB checkpoint | 129.9 ms | 636.2 MiB/s median transfer; p95 1.443 s |
| 256 MiB checkpoint | 521.5 ms | 633.9 MiB/s median transfer; p95 5.876 s |
| 1 GiB checkpoint | 1.641 s | 805.7 MiB/s median transfer; p95 1.697 s |
| 64 MiB OS-warm recovery | 2.055 s | 42.11 MiB/s median replay; p95 2.536 s |
| 256 MiB OS-warm recovery | 11.207 s | 30.90 MiB/s median replay; p95 16.234 s |
| 1 GiB OS-warm recovery | 88.508 s | 15.65 MiB/s median replay; p95 98.862 s |
| Steady-state churn | 17,670 operations/s | 2.219× file amplification; zero measured file growth per round |

The long run makes several trends visible. Batching 256 writes delivered about
39 times the median throughput of single-write commits while reducing WAL
amplification from 31.69× to 1.364×. Point-read throughput fell by about 12.5
times between an engine-hot working set and one sized at 32 times the cache.
Four readers produced the highest measured aggregate concurrent-read rate;
eight readers reduced both reader and writer throughput. Recovery throughput
also declined as the WAL grew, from 42.11 MiB/s at 64 MiB to 15.65 MiB/s at
1 GiB.

Two measurements are retained as diagnostics rather than reference numbers.
Sequential batch-16 writes moved between distinct performance phases, ranging
from 765 to 8,278 updates/s across trials, so their aggregate median is not
representative. Mixed-workload WAL amplification accumulated across trials and
reset after WAL rotation; that metric has an accounting defect and is omitted
from the table. The checkpoint p95 values likewise show substantial host or
storage tail events. As with the smoke run, recovery is OS-cache-warm, and the
unisolated `powersave` host makes this a diagnostic baseline rather than a
controlled release comparison.

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
