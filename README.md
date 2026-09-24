# TinyDB

<p align="center">
  <img src="assets/tinydb-mascot-clean.png" alt="TinyDB pixel mascot" width="420">
</p>

TinyDB is a small, embeddable key-value storage engine. It is meant to run
inside an application's process as a layer for durable storage.

## Build and run

Requires Linux, CMake 3.24 or newer, and a C++23 compiler and standard library with support for std::expected and std::format.

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

## Using TinyDB from SQL

TinyDB can serve as the storage layer beneath a SQL engine. The
[sqlite/](sqlite/) directory contains a SQLite
[virtual table](https://www.sqlite.org/vtab.html) module: SQLite parses,
plans, and executes queries, and TinyDB stores the rows. Building it requires
SQLite's development package (`libsqlite3-dev` on Ubuntu):

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DTINYDB_SQLITE=ON
cmake --build build-release --target tinydb_sqlite_extension -j
```

Load the extension in the `sqlite3` shell and create tables backed by a TinyDB
file:

```text
sqlite3 schema.db
sqlite> .load ./build-release/tinydb
sqlite> CREATE VIRTUAL TABLE users USING tinydb('data.tinydb',
   ...>   id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER);
sqlite> INSERT INTO users VALUES (1, 'alice', 31), (2, 'bob', 27);
sqlite> SELECT name FROM users WHERE id >= 2;
```

SQLite keeps the table definitions in its own file (`schema.db`); the rows live
in `data.tinydb`, keyed by table name and primary key. Programs can link the
`tinydb_sqlite` library instead and call `tinydb_sqlite_register(db)` on a
connection. [examples/sqlite_example.cpp](examples/sqlite_example.cpp) shows
this with bound parameters, joins, transactions, constraint errors, and
reopening. It stores `schema.db` and `data.tinydb` in the directory you supply:

```sh
cmake --build build-release --target tinydb_sqlite_example
./build-release/tinydb_sqlite_example sqlite-example
```

Queries, joins, transactions, and savepoints work as in SQLite. Lookups and
range scans on the primary key use TinyDB's B+ tree, and a failed statement
rolls back only its own changes. The module has these limitations:

- Each table needs a single `INTEGER` or `TEXT` primary key. `INTEGER` keys
  accept only integers.
- SQLite does not support `CREATE INDEX` on virtual tables, so filters on
  other columns scan the table.
- `NOT NULL` is enforced. `CHECK`, `DEFAULT`, `UNIQUE`, and foreign keys are
  rejected when the table is created, because SQLite ignores them on virtual
  tables.
- A row, including its key, must fit in 1 KiB.
- Only one SQLite connection may use a TinyDB file at a time.

Queries pass through SQLite's virtual table interface, so they are slower than
on a native SQLite table: in a quick test with 10,000 rows, 200,000 primary key
lookups took 2.5 times as long.

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
| Sequential inserts | 135,065 (132,166–135,716) | 2.87069 |
| Random inserts | 46,904 (43,125–48,007) | 4.73207 |
| Existing-key reads | 1,789,500 (850,594–1,932,324) | 0.00145 |
| Missing-key reads | 1,870,730 (1,840,384–1,943,654) | 0.00142 |
| 100-entry scans | 23,163,859 (15,153,834–34,537,304) | 0.01101 |
| Full scans | 17,599,530 (11,152,524–25,343,660) | — |
| Overwrites | 51,191 (51,176–51,530) | 3.41459 |
| Deletes | 49,980 (39,346–50,772) | 3.45452 |
| Reinserts | 51,768 (51,516–52,289) | 3.36617 |
| Reads with one active writer | 576,327 (545,171–599,438) | 0.02611 |
| Writes with four active readers | 33,902 (29,972–34,073) | 5.90125 |

### Comparing with SQLite

TinyDB had higher throughput in every workload. Its lead was largest for
point reads and scans; for writes it was 1.14–1.22×. TinyDB used 8.4% less
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
| Sequential inserts | 133,694 ops/s | 117,014 ops/s | TinyDB 1.14× |
| Random inserts | 46,721 ops/s | 40,485 ops/s | TinyDB 1.15× |
| Existing-key reads | 1,869,234 ops/s | 525,500 ops/s | TinyDB 3.56× |
| Missing-key reads | 1,976,036 ops/s | 561,423 ops/s | TinyDB 3.52× |
| 100-entry scans | 501,334 scans/s | 68,659 scans/s | TinyDB 7.30× |
| Full scans | 20,652,847 entries/s | 7,304,800 entries/s | TinyDB 2.83× |
| Overwrites | 47,951 ops/s | 40,844 ops/s | TinyDB 1.17× |
| Deletes | 51,539 ops/s | 42,629 ops/s | TinyDB 1.21× |
| Reinserts | 52,346 ops/s | 43,028 ops/s | TinyDB 1.22× |

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
