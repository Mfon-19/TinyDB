# TinyDB benchmark suite

This suite measures TinyDB itself. It is intended for performance regression
work, capacity studies, and design experiments; it does not make a
cross-database performance claim.

## Profiles

| Profile | Purpose | Trials and warmups | CPU trial floor | Commit samples |
|---|---|---:|---:|---:|
| `smoke` | Validate the harness and catch large regressions | 3 + 1 | 100 ms | 24 per write scenario |
| `standard` | Normal optimization and release measurements | 15 + 3 | 1 s | 1,080 per write scenario |
| `soak` | Tail latency, sustained churn, and large-data work | 30 + 5 | 5 s | 3,840 per write scenario |

The standard matrix covers:

- sequential and random inserts at batch sizes 1, 16, and 256;
- random overwrites, 32-byte through 1 MiB values, and 16- and 256-byte keys;
- engine-cache-hot, eviction-heavy, hotspot, missing-key, and large-value reads;
- one-row, short-range, and full scans, with and without value copying;
- an 80% read / 12% update / 4% insert / 4% delete mixture;
- one writer with 0, 1, 4, and 8 concurrent readers;
- checkpoint and process-restart recovery at 16, 64, and 256 MiB;
- repeated delete/reinsert churn after warmup; and
- point reads with logical working sets near 0.5, 8, and 32 times the cache target.

The soak profile uses the same matrix with longer trials, larger cache and
lifecycle fixtures, and more churn rounds. It is intentionally expensive.

## Running the suite

Build a Release binary:

```sh
cmake -S . -B build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DTINYDB_BUILD_BENCHMARKS=ON
cmake --build build/bench --target TinyDB_bench
```

Run the standard suite, or use smoke before committing harness changes:

```sh
build/bench/TinyDB_bench --profile standard
build/bench/TinyDB_bench --profile smoke
```

Scenario order is randomized from the recorded seed. Selection and diagnostic
overrides are available without changing source:

```sh
build/bench/TinyDB_bench --profile standard --list
build/bench/TinyDB_bench --profile standard --family reads
build/bench/TinyDB_bench --profile standard --filter read.eviction.hotspot
build/bench/TinyDB_bench --profile smoke --ordered --output /tmp/tinydb-bench
```

`--trials`, `--warmups`, `--minimum-trial-ms`, and `--cache-bytes` are intended
for harness development. Published results should use an unchanged named
profile so another run has the same geometry.

## Artifacts

Each run creates a timestamped directory under `benchmark-results/` unless
`--output` is supplied:

```text
metadata.json  machine, build, command, seed, scenario geometry, and run order
samples.csv    every measured trial, commit latency, or churn round
summary.csv    mean, sample deviation, min, p50, p95, p99, and max
```

Fixture creation and warmups are outside measured regions. CPU workloads repeat
complete operation plans until the profile's duration floor is reached. Write
workloads retain each synchronous commit latency rather than inferring a tail
from trial averages. Every workload validates returned data or final state.

The terms in scenario names are narrow:

- `engine_hot` means the logical fixture is sized to fit TinyDB's page-cache
  target and has been warmed before measurement.
- `eviction` means the logical fixture exceeds TinyDB's page-cache target.
- `recovery.os_warm` is a process restart. The writer exits without closing
  TinyDB, but the harness does not claim to evict the operating system's page
  cache or the device cache.

No scenario is called a cold-device benchmark. Reproducible cold-device tests
require machine-specific cache control and should be run by a dedicated host
harness.

## Comparing revisions

Build the baseline and candidate in separate worktrees, then use the comparison
runner:

```sh
python3 bench/compare.py /path/to/baseline/TinyDB_bench \
  /path/to/candidate/TinyDB_bench --profile standard
```

It randomizes scenario order, alternates whether baseline or candidate runs
first, and uses identical seeds within each pair. Two repetitions counteract
first-run and temporal drift by default. It preserves all child artifacts and
writes:

```text
samples.csv       combined raw observations with variant and repetition
comparison.csv    per-trial medians, effect sizes, and bootstrap ratio intervals
metadata.json     binaries, profile, seed, repetitions, and scenario list
runs/             the complete artifacts from every child run
```

An `inconclusive` comparison means the 95% bootstrap interval crosses parity;
it does not prove the revisions are equal. Inspect the raw trial distribution,
system activity, and effect size before treating any result as actionable.

For serious measurements, pin the machine's power policy, record storage and
filesystem configuration, stop unrelated workloads, ensure sufficient free
space, and compare revisions on the same host. Do not mix sanitizer or Debug
builds with Release results.
