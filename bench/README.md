# TinyDB benchmarks

The benchmark has two parts:

- a C++ worker that runs workloads against one backend; and
- `runner.py`, which builds fixtures, repeats trials, compares results, and
  writes the report.

The same worker sources build against TinyDB, SQLite, LevelDB, or RocksDB.
TinyDB builds also include a small engine-specific qualification suite.

## Commands

```sh
make bench
make bench-compare
```

`make bench` measures the current checkout. `make bench-compare` compares it
with `direct-io` (override that with `DIRECT_IO_REVISION`).

Useful focused runs:

```sh
make bench BENCH_PROFILE=smoke BENCH_ARGS='--family db_bench'
make bench BENCH_ARGS='--family ycsb'
make bench-compare BENCH_ARGS='--filter readrandom'
make bench-compare CACHE_MIB=8
make bench BENCH_OUTPUT=/tmp/tinydb-buffered
```

The normal TinyDB cache is 16 MiB. `CACHE_MIB` overrides both sides of a
comparison; `BASELINE_CACHE_MIB` and `DIRECT_CACHE_MIB` override one side.

## Workloads

The default portable family uses the familiar db_bench names:

- sequential, random, and batch fill;
- overwrite;
- random point reads, sequential reads, and random seeks; and
- random delete.

YCSB A-F are available with `--family ycsb`. They use the standard read,
update, insert, short-scan, and read-modify-write mixes with a Zipfian key
distribution. These are native C++ workloads, not RocksDB's executable or a
Java YCSB client, so every backend pays the same harness cost.

Portable sizes come from `--profile`:

| Profile | db writes | db read records / operations | YCSB records / operations | Trials |
|---|---:|---:|---:|---:|
| `smoke` | 2,000 | 2,000 / 5,000 | 2,000 / 2,000 | 1 |
| `standard` | 20,000 | 250,000 / 250,000 | 50,000 / 50,000 | 3 |
| `soak` | 200,000 | 2,000,000 / 2,000,000 | 500,000 / 1,000,000 | 5 |

TinyDB qualification adds the behavior a generic key/value interface cannot
measure:

- hot-cache, eviction-heavy, range, and large-value cases;
- concurrent readers with a writer;
- checkpoint, WAL recovery, and delete/reinsert churn;
- verified-cold sequential, random, and overflow-value reads; and
- direct-I/O prefetch activity, usefulness, drops, failures, and memory.

## Fair comparisons

Each scenario is built once per compatible on-disk format. Buffered and direct
TinyDB therefore start from byte-identical canonical files. Other engines get
the same logical keys and values in their own format. Every trial receives a
private byte-for-byte copy, which is hashed before use and deleted afterwards.

Engines run sequentially in balanced rotating order. Scenario order, operation
plans, and trial seeds are deterministic. Setup, cache priming, and validation
are outside the timed region.

`durable` is the default: TinyDB keeps its normal commit boundary, SQLite uses
`synchronous=FULL`, and LevelDB/RocksDB synchronize measured writes.
`--semantics native` uses each external engine's normal asynchronous setting
and must not be read as an equal-durability comparison.

The report uses paired trial ratios. Positive metrics receive a 95% Student
interval over log ratios; only a scenario's declared primary metric receives a
verdict. A one-trial smoke run has a point estimate but no confidence interval.

## Results

A successful run contains only:

```text
report.md       human-readable tables
results.csv     every raw trial and nested observation
metadata.json  provenance, matrix, ordering, seeds, and fixture hashes
```

Fixtures and trial copies are temporary. Workers are not copied into the
result; their resolved paths, SHA-256 hashes, exact revisions, dirty state,
compiler, and build type are recorded, and the runner verifies that no binary
changed during the run.

`results.csv` is the source of truth. Every median, effect, and confidence
interval in `report.md` can be recomputed from it; there are no duplicate
summary CSVs.

The default result is `/tmp/tinydb-benchmark-latest`. It replaces the previous
result only after all three files are complete. Use `--output` (or the Make
variables above) to retain a named result.

For cross-engine runs, the ignored `build/local-crossbench/run` launcher builds
the backend workers and delegates to this same runner.
