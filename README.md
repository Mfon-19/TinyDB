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

The benchmark worker has one operation protocol, result schema, metrics
implementation, and runner. Its portable workloads use db_bench conventions
and native YCSB A-F mixes; backend adapters let the same code measure TinyDB,
SQLite, LevelDB, and RocksDB. TinyDB builds additionally expose cold-I/O,
checkpoint, recovery, churn, and concurrent-reader qualification workloads.

Measure the current checkout:

```sh
make bench
```

The command builds a Release worker and replaces
`/tmp/tinydb-benchmark-latest` only after a complete successful run. Fixture
construction is outside timed regions, and every trial receives a private
verified copy. Reports include call latency, CPU, PSS, database-file residency,
physical I/O, persistent size, and TinyDB prefetch diagnostics. Each result is
just `report.md`, raw `results.csv`, and `metadata.json`; fixture and trial
files are deleted when the run completes.

### Buffered-I/O baseline

The following results came from `make bench` at `adcd08b` using a Release
build, the standard profile, durable commits, and a 16 MiB TinyDB page cache.
They are medians from three trials, except the concurrent-reader workload,
which uses five. The complete run took 10.2 minutes.

| Area | Workload | Median |
|---|---|---:|
| Writes | Sequential durable insert | 651 records/s |
| Writes | Random durable insert | 618 records/s |
| Writes | Batch-16 durable insert | 10,119 records/s |
| Writes | Overwrite | 621 records/s |
| Writes | Delete | 625 records/s |
| Writes | 64 KiB values, batch four | 508 records/s |
| Reads | Random point read | 574,858 reads/s |
| Reads | Sequential scan | 5,460,605 rows/s |
| Reads | Random seek | 497,197 seeks/s |
| Cache | Fully engine-hot random read | 2,184,097 reads/s |
| Cache | Eviction-heavy random read | 376,657 reads/s |
| Cache | 256-row ranges | 613,529 rows/s |
| Concurrency | Four readers with one writer | 397,021 reads/s |
| Lifecycle | 64 MiB checkpoint | 90.98 ms |
| Lifecycle | OS-cache-warm recovery | 1.085 s |
| Lifecycle | Delete/checkpoint/reinsert churn | 102,159 operations/s |
| Cold I/O | Sequential scan | 981,121 rows/s |
| Cold I/O | Random point read | 11,506 reads/s |
| Cold I/O | 64 KiB values, compatible layout | 15,873 values/s |
| Cold I/O | 64 KiB values, native layout | 15,981 values/s |

Batching sixteen small writes improved record throughput by 15.5×. Buffered
I/O also makes the double-cache cost visible: the eviction workload occupied
19.88 MiB of engine PSS plus 116.18 MiB of Linux file cache, while the range
workload reached 183.45 MiB combined. Median physical writes were 17.86 MiB for
the batch-16 insert, 169.34 MiB for sequential single inserts, 249.15 MiB for
random single inserts, 2.17 GiB for overwrite, and 2.18 GiB for delete.

These are host-specific results from an Intel Core i5-1135G7, Linux 6.17, and
the `powersave` CPU governor. Recovery was noisy across its three trials
(365 ms to 1.769 s), so small recovery changes require additional trials
rather than relying on this median alone.

See [the benchmark guide](bench/README.md) for profiles, focused commands,
workload semantics, cache control, and artifacts. Results from the retired
TinyDB-only CRUD matrix are not directly comparable.

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
