# bench — Benchmarks

Performance is part of what makes a storage engine impressive, and benchmarking is
where the project's systems/architecture story gets told. These programs load the
engine, measure it, and report numbers tied back to hardware behavior.

## What to measure
- **Write throughput** — bulk-load N keys; report inserts/sec and final DB size.
- **Point read latency** — `get` p50 / p99 (microseconds), warm vs cold cache.
- **Range scan throughput** — keys/sec for `scan` over large ranges.
- **Buffer-pool hit rate** — under various pool sizes and access patterns.
- **Recovery time** — how long `open()` takes to replay the WAL after a crash.
- **fsync cost** — sync-per-op vs batched, to make the durability trade-off visible.

## Example headline numbers (the kind to publish in the README)
```
Inserted 1,000,000 keys      DB size: 128 MB
get p50 / p99:               X µs / Y µs
scan throughput:             Z keys/sec
buffer-pool hit rate:        94%
crash recovery time:         R ms
```

## Planned files
- `bench_load.cpp` — bulk insert, write throughput + DB size.
- `bench_point.cpp` — random `get` latency distribution.
- `bench_scan.cpp` — range-scan throughput.
- `bench_recovery.cpp` — crash, then time recovery on reopen.
- `harness.h` — timing, percentile, and reporting helpers.

## Key decisions
- Report **distributions (p50/p99), not just averages** — tail latency is what
  matters and what shows benchmarking maturity.
- Tie results to hardware: page size vs branching factor, random vs sequential
  SSD I/O, cache-friendly record layout, fsync cost.
- Benchmarks link against the **release** preset (optimized, no sanitizers).
