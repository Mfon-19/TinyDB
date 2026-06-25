# TinyDB

A disk-backed, ordered **key-value storage engine** in C++ — a page-based file
format, a buffer pool, a B+-tree index, write-ahead logging, and crash recovery,
behind a small embedded API.

It is the low-level persistence core that sits *underneath* a database — not a
full RDBMS. No SQL, query planner, joins, or network server; just the hard,
systems-heavy part: storage, indexing, caching, and durability.

```cpp
tinydb::StorageEngine db("data.db");

db.Put("user:1", "Mfon");
db.Put("user:2", "Alice");

auto v = db.Get("user:1");          // -> "Mfon"
db.Remove("user:2");

for (auto [key, value] : db.Scan("user:1", "user:9"))
    std::cout << key << " = " << value << "\n";
```

## Architecture

```
StorageEngine API   (Put / Get / Remove / Scan)   src/engine
   │
B+-tree index                                      src/btree
   │
Buffer pool (page cache)                           src/buffer
   │
DiskManager / page file                            src/storage
   │            ▲ write-ahead rule
WAL ──► Recovery │                                 src/wal, src/recovery
   │
Linux fd I/O (pread / pwrite / fdatasync)          src/storage
   │
disk
```

`src/codec` holds the shared byte-encoding primitives (slices, varints, cells).

## Layout

| Path | What's there |
|------|--------------|
| `src/storage` | DiskManager, page-based file format, freelist, Linux fd I/O |
| `src/buffer`  | Buffer pool / page cache (pin, dirty, eviction) |
| `src/btree`   | B+-tree index — ordered Get / Put / Delete / Scan |
| `src/codec`   | Encoding primitives: `Slice`, varints, cell format |
| `src/wal`     | Write-ahead log (redo logging) |
| `src/recovery`| Crash recovery (redo replay) + checkpointing |
| `src/engine`  | The storage-engine API and lifecycle |
| `src/cli`     | Interactive REPL for the KV API |
| `include/tinydb` | Public headers (the embed surface) |
| `tests`       | Unit, differential (vs `std::map`), and chaos tests |
| `bench`       | Throughput / latency / recovery benchmarks |
| `docs`        | Design, style, tooling, file/WAL format, recovery |

See [`docs/`](docs/) for the design, and [`docs/STYLE.md`](docs/STYLE.md) /
[`docs/TOOLING.md`](docs/TOOLING.md) for engineering conventions.

## Status

Early design / scaffolding. Each folder's `README.md` describes what will live
there; `docs/ROADMAP.md` tracks milestones.
