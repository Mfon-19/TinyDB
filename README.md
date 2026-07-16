# TinyDB

TinyDB is a single-process, ordered, transactional key-value database embedded
directly in a C++23 application. It stores byte-string keys and values in a
checksummed B+ tree, commits through a physical write-ahead log, and recovers
automatically when the database is reopened after a crash.

```cpp
#include <tinydb/database.h>

auto database = tinydb::Database::Open("notes.db").value();

auto write = database.BeginWrite().value();
if (!write.Put("doc/1", "contents").Ok() ||
    !write.Put("tag/database/doc/1", "").Ok()) {
  write.Abort();
  return;
}
auto committed = std::move(write).Commit();
if (!committed) {
  return;
}

auto read = database.BeginRead().value();
auto cursor = read.Scan(tinydb::KeyRange::Prefix("tag/database/")).value();
while (cursor.Valid()) {
  Use(cursor.Key(), cursor.CopyValue().value());
  if (!cursor.Next().Ok()) {
    break;
  }
}
```

## Contract

- Keys are unique byte strings ordered by unsigned lexicographic order.
- `Put` inserts or replaces, and `Delete` is idempotent.
- A write transaction publishes all of its mutations or none of them.
- A successful commit is durable without `Close` or an explicit checkpoint.
- An indeterminate durability failure moves the handle to `NeedsRecovery`; the
  application must reopen before determining the transaction outcome.
- Read transactions and their cursors retain one stable committed snapshot.
- Many readers may coexist. One write transaction may prepare at a time.
- One process owns a database file at a time through an exclusive file lock.
- Keys are limited to 1 KiB and values to 4 MiB. Large values use checksummed
  overflow pages and are copied by `Get` and `Cursor::CopyValue`.
- Detected malformed persistent state is returned as `Corruption` or
  `UnsupportedFormat`; it is not treated as an internal assertion failure.

TinyDB does not provide SQL, schemas, networking, replication, multi-process
readers, or concurrent write transactions.

## Storage model

```text
application
    │
    ├── ReadTransaction ── immutable committed snapshot ──┐
    │                                                     │
    └── WriteTransaction ── private mutable page overlay  │
                              │                           │
                              ├── fsynced physical WAL    │
                              └── atomic publication ─────┘
                                           │
                                  committed page cache
                                           │ checkpoint
                                  checksummed page file
```

Before WAL synchronization, write pages are private and discardable. The
commit path prepares every allocation required for visibility, appends final
page images and resulting roots, synchronizes the WAL, drains older readers,
and publishes without further allocation or I/O. Checkpointing later writes
those immutable committed versions to the database file and removes covered
WAL segments. It is not a commit boundary.

The database file uses explicit little-endian encodings, per-page checksums,
and alternating checksummed superblocks. The WAL is segmented, checksummed,
bound to the database UUID, and replays only complete self-binding
transactions.

## Build and test

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Sanitizer and clang-tidy presets are defined in `CMakePresets.json`. Benchmarks
are optional:

```sh
cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DTINYDB_BUILD_BENCHMARKS=ON
cmake --build build/bench --target TinyDB_workloads_bench
```

No historical performance figures are claimed here; storage, filesystem, and
durability settings materially change the result.

## Install and consume

```sh
cmake --install build/release --prefix /your/prefix
```

A downstream CMake project can then use only the installed public boundary:

```cmake
find_package(TinyDB CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE TinyDB::TinyDB)
```

The installed headers are:

```text
tinydb/bytes.h
tinydb/cursor.h
tinydb/database.h
tinydb/options.h
tinydb/stats.h
tinydb/status.h
tinydb/transaction.h
```

Page formats, the cache, WAL, allocator, recovery, and B+ tree types are
private implementation details under `src/`.

## Command line

The `tinydb` executable performs one operation per invocation:

```text
tinydb <database> put <key> <value>
tinydb <database> get <key>
tinydb <database> del <key>
tinydb <database> scan
tinydb <database> scan <lower> <upper>
```

The CLI is a thin demonstration of the public API. It is not a server or an
interactive shell.

## Repository layout

| Path | Responsibility |
|---|---|
| `include/tinydb` | Installed application API |
| `src/api` | Handle, transaction, lifecycle, and publication coordination |
| `src/btree` | Ordered index, cursors, and overflow values |
| `src/cache` | Immutable committed page versions |
| `src/txn` | Reader gate, private overlay, allocator, and commit protocol |
| `src/wal` | Segmented WAL and physical transaction codec |
| `src/recovery` | WAL validation and idempotent redo |
| `src/checkpoint` | Immutable checkpoint capture and cleanup |
| `src/storage` | Database file, superblocks, and page codecs |
| `src/io` | POSIX I/O boundary and fault-injection hooks |
| `tests` | Contract, model, corruption, durability, and crash tests |
