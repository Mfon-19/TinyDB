# src/engine — Storage engine API

The top of the engine: the component that wires the layers together and exposes
the public **key-value API**. This is what an embedding application (or the CLI)
talks to. It owns the lifecycle — open the file, run recovery, serve operations,
checkpoint, close — and delegates the real work downward.

## The API (v1)
```cpp
StorageEngine db("data.db");

db.put(key, value);            // insert or overwrite
auto v = db.get(key);          // -> optional<value>
db.remove(key);                // delete (tombstone)
for (auto [k, v] : db.scan(lo, hi))   // ordered range scan
    ...
```
Keys and values are opaque byte strings (`Slice`). No SQL, no schema, no joins —
that surface area is deliberately out of scope (see `docs/ROADMAP.md`).

## Responsibilities
- **Open**: initialize `DiskManager`/buffer pool/WAL, then run `src/recovery`
  before accepting any request.
- Implement `put` / `get` / `remove` / `scan` on top of the B+-tree.
- Enforce **per-operation atomicity + durability**: log the change, durably sync
  the log to commit, then it's safe — a crash either fully applies the op or not
  at all.
- Drive **checkpointing** and clean **close** (flush + durable sync + mark clean).

## Planned files
- `storage_engine.h` / `storage_engine.cpp` — `StorageEngine`: owns
  `DiskManager`/`BufferPool`/`Wal`/`BTree`; implements the KV API and lifecycle.
- `options.h` — open options (path, buffer-pool size, sync mode).

## Key decisions
- **Embedded library**, not a server — no networking/auth/connections (the
  SQLite/LMDB/RocksDB shape).
- Atomicity is **per operation** for v1; multi-op transactions
  (`begin`/`commit`/`rollback`) are a documented later extension.
- The public types are mirrored in `include/TinyDB` — this folder is the impl;
  embedders include only the public headers.
