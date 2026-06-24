# src/wal — Write-ahead log

The durability layer. Before any page change reaches the database file, a record
describing it is appended to the **write-ahead log** and made durable. This is
what lets the engine survive a crash: the log is the authoritative record of
recent changes, replayed at startup by `src/recovery`.

## The write-ahead rule (the invariant everything depends on)
> A modified page may not be written to the database file until the log records
> describing that change are durable on disk.

The buffer pool (`src/buffer`) honors this: it asks the WAL to flush up to a
page's log sequence number before flushing that dirty page.

## Durability strategy: redo logging (Option A)
- **Redo-only** for v1: log the new state of each change; on recovery, **replay**
  committed records forward.
- Single-writer model means there is no uncommitted-then-rolled-back state to
  undo — this is the deliberate simplification that keeps recovery tractable.
- Each operation (`put`/`delete`) is logged, then committed by a durable sync of
  the log. A committed op is durable even if the data pages aren't flushed yet.

## Log format
- Append-only file (or log pages). Each record: type, **log sequence number
  (LSN)**, payload (e.g. page image or logical op), and a **checksum** to detect
  torn/partial writes at the tail.
- A **commit record** marks an operation as durable.

## Planned files
- `wal.h` / `wal.cpp` — `Wal`: `append(record) -> Lsn`, `flush_to(lsn)`
  (`fdatasync`/`fsync`), `iterate()` for recovery.
- `log_record.h` / `log_record.cpp` — record types + (de)serialization + checksum.
- `lsn.h` — the log sequence number type.

## Key decisions
- **Redo-only, single-writer** — the simplest design that still gives real crash
  recovery. Undo logging / full ARIES is a documented later extension.
- Record checksums so recovery can detect and stop at a torn final write.
- Checkpointing (truncating the log once pages are safely flushed) lands with
  `src/recovery`.
