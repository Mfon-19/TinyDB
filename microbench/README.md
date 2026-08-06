# TinyDB microbenchmarks

This suite measures one in-memory TinyDB component at a time. It uses Google Benchmark and private TinyDB headers.

The timed loops do not perform storage I/O. Use `bench/` for database workloads, durability, recovery, and cold storage.

## Run the suite

Run the stable default measurement:

```sh
make microbench
```

The command uses ten repetitions, random interleaving, and a minimum time of 0.1 seconds.

Run one benchmark family:

```sh
make microbench MICROBENCH_ARGS='--benchmark_filter=Overflow'
```

List all benchmark names:

```sh
make microbench-build
build/microbench/TinyDB_microbench --benchmark_list_tests
```

Write a JSON result:

```sh
build/microbench/TinyDB_microbench \
  --benchmark_repetitions=10 \
  --benchmark_enable_random_interleaving=true \
  --benchmark_out=microbench.json \
  --benchmark_out_format=json
```

## Measurement contract

- Each benchmark builds its fixture before the timed loop.
- Each query sequence is deterministic and contains varied keys.
- A state-changing benchmark restores the required state before each measurement.
- An owning production API includes its allocation and copy cost.
- The benchmark uses `DoNotOptimize` and `ClobberMemory` where the compiler can remove work.
- Byte-oriented benchmarks report the number of processed bytes.
- Cache benchmarks report resident hits only. They do not report physical misses.
- The output records the TinyDB commit, dirty state, build type, compiler, CPU, and page size.

## Families

| Family | Measured work |
|---|---|
| `CRC32` | Contiguous checksums, a zeroed field, checksum combination, and checksum replacement |
| `PageCodec` | Page finalization, LSN rewrite, header decode, and overflow-page decode |
| `PageView` | Full page opening, proved page opening, leaf search, point lookup, and internal routing |
| `Overflow` | Overflow-chain reads with and without retained header proofs |
| `OverflowRawCopyControl` | A benchmark-only copy control without page traversal or validation |
| `WalCodec` | Transaction encoding with and without page proofs, plus transaction decoding |
| `Cache` | Resident committed-cache hits for one page and a rotating working set |

## Interpret results

Compare results from the same machine, compiler, build type, and power configuration. Use Release builds for performance decisions.

Examine the coefficient of variation from repeated runs. Treat a result above 5% as noisy.

Do not use shared-host results as a merge gate. First use this suite to locate a change.

Then use `bench/` to measure the database effect.

## Sample result: 2026-08-06

These measurements came from a local run on 2026-08-06. The run used GCC 13.3.0, a Release build, 4 KiB pages, and ten repetitions.

CPU scaling was active, so small differences are not reliable. The tables use the mean time and the reported coefficient of variation (CV).

These numbers are a local reference, not a performance guarantee. They describe hot, in-memory component work without storage I/O.

### Main results

| Area | Case | Mean time | Throughput | CV |
|---|---|---:|---:|---:|
| CRC32 | 4 KiB | 1.322 µs | 2.89 GiB/s | 4.15% |
| CRC32 with a zeroed field | 4 KiB | 1.283 µs | 2.98 GiB/s | 5.51% |
| CRC32 combine | Fixed shift | 12.7 ns | 79.4M operations/s | 7.13% |
| CRC32 replace | Fixed range | 93.7 ns | 10.7M operations/s | 3.57% |
| Page codec | Decode an unproved header | 1.360 µs | 736.8K pages/s | 4.39% |
| Page codec | Decode a proved overflow page | 9.92 ns | 101.0M pages/s | 3.84% |
| Page view | Open a raw leaf with 128 records | 1.938 µs | 517.1K operations/s | 4.67% |
| Page view | Open a proved leaf with 128 records | 3.33 ns | 300.8M operations/s | 3.55% |
| Leaf lookup | Get a hit from 128 records | 29.0 ns | 34.6M operations/s | 4.25% |
| Overflow value | Proved, 64 pages | 7.042 µs | 34.2 GiB/s | 3.04% |
| Overflow value | Unproved, 64 pages | 94.387 µs | 2.55 GiB/s | 2.56% |
| WAL encode | Proved, 16 pages | 4.618 µs | 13.35 GiB/s | 5.54% |
| WAL encode | Fallback, 16 pages | 46.060 µs | 1.34 GiB/s | 2.87% |
| Cache hit | MRU, one thread | 27.6 ns | 36.3M operations/s | 4.15% |
| Cache hit | MRU, eight threads | 1.512 µs | 662K operations/s | 3.75% |

### Overflow value results

| Pages | Raw-copy control | Proved path | Unproved path | Added unproved cost | Slowdown |
|---:|---:|---:|---:|---:|---:|
| 1 | 0.202 µs | 0.105 µs | 1.491 µs | 1.386 µs | 14.2 times |
| 4 | 0.503 µs | 0.257 µs | 5.718 µs | 5.461 µs | 22.3 times |
| 16 | 2.222 µs | 1.741 µs | 22.541 µs | 20.800 µs | 12.9 times |
| 64 | 8.555 µs | 7.042 µs | 94.387 µs | 87.345 µs | 13.4 times |

The unproved path adds approximately 1.35 µs for each page. This cost closely matches the 1.322 µs CRC32 time for one 4 KiB page.

Thus, page authentication causes most of the extra time. For 64 pages, retained proofs save approximately 87 µs for each value read.

The raw-copy control is slower than the proved path. Therefore, it is not an exact lower bound for the copy cost.

### Page view results

| Operation | 16 records | 64 records | 128 records |
|---|---:|---:|---:|
| Open a raw leaf | 1.477 µs | 1.710 µs | 1.938 µs |
| Open a proved leaf | 3.32 ns | 3.31 ns | 3.33 ns |
| Open a raw internal page | 1.512 µs | 1.763 µs | 2.044 µs |
| Open a proved internal page | 3.28 ns | 3.40 ns | 3.27 ns |
| Leaf `LowerBound`, hit | 18.3 ns | 25.8 ns | 30.7 ns |
| Leaf `LowerBound`, miss | 18.7 ns | 27.5 ns | 34.0 ns |
| Leaf `Get`, hit | 16.4 ns | 24.2 ns | 29.0 ns |
| Leaf `Get`, miss | 19.4 ns | 27.2 ns | 32.5 ns |
| Internal child, hit | 14.7 ns | 20.2 ns | 23.8 ns |
| Internal child, between keys | 17.4 ns | 23.9 ns | 28.0 ns |

Lookup time increases slowly as record count increases. An eightfold record increase raises lookup time by approximately 1.6 to 1.8 times.

Raw page opening authenticates and parses the page. Proved opening reuses trusted metadata and takes approximately 3.3 ns.

The raw-to-proved difference is approximately 450 to 625 times. This result shows the value of retained page proofs on cache hits.

### WAL results

| Pages | Proved encode | Fallback encode | Decode | Proved speedup |
|---:|---:|---:|---:|---:|
| 1 | 0.333 µs | 2.963 µs | 2.923 µs | 8.9 times |
| 4 | 1.079 µs | 11.383 µs | 12.117 µs | 10.6 times |
| 16 | 4.618 µs | 46.060 µs | 48.707 µs | 10.0 times |

The proved WAL encoder is approximately ten times faster than the fallback encoder. This result supports the current CRC32 combine and replace operations.

### Cache results

| Resident-cache workload | One thread | Eight threads | Approximate degradation |
|---|---:|---:|---:|
| MRU hit | 27.6 ns | 1.512 µs | 55 times |
| 256-page round robin | 27.1 ns | 2.289 µs | 84 times |

The cache benchmark uses resident pages and performs no storage I/O. The eight-thread results indicate contention in cache locking or LRU maintenance.

The one-thread round-robin result has a 20.5% CV. Therefore, its exact value is not reliable.
