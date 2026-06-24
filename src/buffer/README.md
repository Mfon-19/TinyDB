# src/buffer — Buffer pool (page cache)

The in-memory cache of disk pages, sitting between the B+-tree and `DiskManager`.
Instead of hitting disk on every access, callers fetch pages here; the pool keeps
hot pages resident, tracks which are dirty, and decides what to evict. One of the
most important — and most benchmarked — components of the engine.

## Responsibilities
- Cache a bounded set of pages in memory (the pool).
- **Pin / unpin** pages so an in-use page is never evicted out from under a caller.
- Track **dirty** pages and flush them back through `DiskManager`.
- **Evict** a victim when the pool is full, using a replacement policy.
- Cooperate with the WAL: a dirty page may only be flushed **after** its log
  records are durable (write-ahead rule, enforced via `src/wal`).

## The fetch path
```
FetchPage(pgno):
  in pool?  → pin, return it (cache hit)
  else      → pick a victim, flush it if dirty (WAL first),
              read pgno from DiskManager, pin, return it (cache miss)
```

## Planned files
- `buffer_pool.h` / `buffer_pool.cpp` — `BufferPool`: `FetchPage(pgno)`,
  `UnpinPage(pgno, dirty)`, `NewPage()`, `FlushPage(pgno)`, `FlushAllPages()`.
- `frame.h` — a pool slot: page buffer, page number, pin count, dirty flag.
- `replacer.h` / `replacer.cpp` — eviction policy (start with **clock**; LRU is
  an easy alternative). Picks an unpinned victim.
- Optional later: `disk_scheduler.h` / `disk_scheduler.cpp` if async I/O becomes
  useful; v1 can call `DiskManager` synchronously.

## Key decisions
- Replacement policy starts as **clock** (cheap, good enough); the interface lets
  it be swapped without touching the pool.
- **Buffer-pool hit rate** is a headline benchmark metric (see `bench/`).
- Latching for concurrent access is deferred (single-writer v1); the pin/dirty
  bookkeeping is designed so it can be added later.
