# TinyDB benchmarks

The benchmark suite answers one question: what does a TinyDB engine revision
cost in throughput, latency, CPU, memory, and storage behavior? It is a
regression and design-comparison tool, not a cross-database ranking.

## Commands

From `main`, the complete public interface is:

```sh
make bench
make bench-compare
```

`make bench` measures the current working tree. `make bench-compare` performs a
paired comparison between that tree and the revision named by
`DIRECT_IO_REVISION`, which defaults to `direct-io`.

The two default commands share `/tmp/tinydb-benchmark-latest`. A successful
run replaces the previous successful default result, so ordinary use retains
only one result. The replacement happens after the new report is complete.

Named archives and focused investigations use Make variables:

```sh
make bench BENCH_OUTPUT=/tmp/buffered-results
make bench-compare COMPARISON_OUTPUT=/tmp/buffered-vs-direct
make bench-compare DIRECT_CACHE_MIB=32
make bench-compare BENCH_ARGS='--family cold_io'
make bench BENCH_ARGS='--filter checkpoint'
```

`DIRECT_CACHE_MIB` changes only the direct-I/O candidate's page cache. The
buffered baseline, fixture construction, workload sizes, and scenario matrix
retain their declared 8 MiB setting, which makes cache-size experiments
comparable.

An explicitly named output is never replaced; choose a new path or remove the
old archive deliberately.

On the reference development host the full paired comparison takes roughly
8–12 minutes; a standalone run is shorter. Runtime depends on the storage
device, especially for synchronous commits and cache-dropped random reads.

## Matrix

The fixed representative matrix contains sixteen scenarios:

- sequential single-row inserts, random batched inserts, random overwrites,
  and 64 KiB values;
- engine-hot and eviction-heavy point reads;
- short value-copying range scans;
- a uniform 80% read / 12% update / 4% insert / 4% delete workload;
- one writer with four readers;
- 64 MiB checkpoint and OS-warm recovery;
- bounded delete/reinsert churn; and
- cache-dropped full scans, random reads, and both shared-layout compatibility
  and native-layout large-value scans.

This is intentionally not a Cartesian product. Variants that previously
repeated the same conclusion were removed. Long endurance questions are
targeted experiments, not a larger version of the whole suite.

Each scenario declares its fixture policy, trial count, cache condition,
primary metric, preferred direction, and meaningful-effect threshold. Stable
workloads use five trials. Cold I/O, lifecycle, and churn use three more
expensive trials.

## One trial contract

The C++ executable has only three operations:

```text
list scenarios
build one canonical fixture
run one trial from an existing fixture
```

The Python runner owns all repetition and A/B orchestration. Every trial:

1. receives a private userspace copy of an immutable canonical database family;
2. performs its scenario-specific preparation outside the timer;
3. establishes and verifies the declared cache condition;
4. measures exactly one independent trial; and
5. writes scalar trial metrics plus explicitly nested observations.

Every copied fixture except an explicitly OS-warm recovery fixture begins with
its file pages evicted and verified cold. Engine-hot and steady read workloads
are then primed through TinyDB's public read API, so buffered I/O naturally
populates Linux's page cache while direct I/O does not inherit irrelevant pages
from fixture copying. Mixed, concurrent, and churn trials also prepare their
own private state before measurement. Cold trials repeat the eviction at the
measurement boundary. Linux must accept `POSIX_FADV_DONTNEED`, and `mincore`
residency must remain below the declared cold limit; an invalid trial is an
error rather than a row that must be inspected manually.

The harness is built from the current tree against each engine source tree.
Consequently both binaries have identical scenario and measurement code even
when the engine revisions live on different branches.

## Paired comparison

Baseline and candidate never run concurrently. Shared-layout scenarios give
both variants the same baseline-built persistent bytes. Native-layout scenarios
let each variant build the same deterministic logical dataset using its own
allocator and format. Every trial pair shares one logical dataset ID and trial
seed, then runs consecutively. First position is randomized in balanced
`AB`/`BA` blocks, and scenario order is randomized once per run.

Every canonical physical family and working copy consists of regular,
single-link files. Each family has its own content-derived ID. Copies use an
explicit read/write loop, are hashed while copied, are synced, and are rejected
if FIEMAP reports shared extents. Trials validate the declared logical dataset
through TinyDB reads before emitting paired results. The exact executables are
also copied and hashed into the result directory before use.

Do not compare two independent `make bench` directories to infer a small
performance difference. The paired runner controls fixture, plan, ordering,
and temporal drift that independent runs cannot control.

## Statistics

The independent experimental unit is a trial pair. For positive scalar
metrics, the report computes the geometric mean of paired candidate/baseline
ratios and a 95% Student interval over their logarithms.

Reported effects preserve the primary metric's units: higher-is-better effects
are throughput gains, while lower-is-better effects are latency reductions.

Only the declared primary metric receives an assessment:

- `improved`: the complete interval exceeds the meaningful threshold;
- `regressed`: the complete interval exceeds it in the wrong direction;
- `equivalent`: the complete interval lies inside the equivalence band; or
- `inconclusive`: more targeted evidence is required.

Commit latencies and churn rounds remain nested observations. Their p50 and
p95 are first reduced to one scalar per trial before any comparison; raw
observations remain available but are never treated as independent trials.
Secondary memory, CPU, I/O, amplification, and size metrics are diagnostic and
do not generate hundreds of multiple-comparison verdicts.

## Artifacts

Every result directory contains:

```text
report.md                 human-readable primary and diagnostic scorecard
samples.csv               all scalar and nested samples
observations.csv          nested commit and churn observations
observation-summary.csv  descriptive nested distributions by variant
summary.csv               standalone trial summaries, for `make bench`
comparison.csv            paired effects and intervals, for `make bench-compare`
metadata.json             binaries, environment, matrix, order, and fixtures
binaries/                 exact archived executables
fixtures/                 immutable canonical database families
runs/                     per-trial samples, metadata, and copy audits
```

Measurements include wall throughput, latency, CPU time, faults and context
switches, `/proc/self/io` bytes and syscall counts, RSS/PSS, TinyDB cache
residency, database-file page-cache residency, persistent size, and read/write
amplification where applicable. Read scenarios additionally report streams,
submitted, ready, waited, bypassed and unused readahead pages, queue and budget
drops, failures, staging memory, and peak in-flight work. Buffered builds emit
the same schema with zero readahead activity.

## Measurement hygiene

Use Release builds on an otherwise idle machine. Keep both variants on the
same filesystem and storage device, hold the CPU power policy stable, and avoid
mixing runs taken under materially different thermal or background-load
conditions. The metadata records the binaries, commits, dirty state, compiler,
kernel, CPU, governor, memory, filesystem, scenario order, and trial seeds.
