# src/recovery — Crash recovery

What turns "writes files" into "is a database." On startup, before serving any
request, recovery reads the write-ahead log (`src/wal`) and brings the database
file back to a consistent state — replaying changes that were committed but may
not have reached their data pages, and discarding any incomplete tail.

This is the component the **chaos test** exists to validate: kill the process at
an arbitrary point, reopen, and the database must come back correct.

## Recovery algorithm (redo)
```
on Open():
  scan the WAL from the last checkpoint
  verify each record's checksum; stop at the first torn/partial record
  for each committed record:
      re-apply its change to the page (idempotent redo)
  discard any records after the last commit (incomplete operation)
  truncate/checkpoint the log
```
Redo is **idempotent** — replaying an already-applied change is harmless — so
recovery is safe to run after a crash *during recovery itself*.

## Responsibilities
- Detect whether recovery is needed (clean vs. dirty shutdown).
- Replay committed WAL records to restore page state.
- Drop the incomplete trailing operation (no half-applied writes survive).
- **Checkpoint**: once dirty pages are safely flushed, advance the log start so
  the WAL doesn't grow without bound.

## Planned files
- `recovery.h` / `recovery.cpp` — `Recover(Wal&, DiskManager&)`: the redo pass
  run at open.
- `checkpoint.h` / `checkpoint.cpp` — flush dirty pages, sync the database file,
  and advance the WAL start point.

## Key decisions
- **Redo-only** recovery (matches the WAL's redo logging) — no undo pass in v1.
- Idempotent replay so recovery survives repeated crashes.
- Validated by **fault-injection / chaos testing**, not just unit tests — this is
  the project's strongest correctness signal (see `tests/`).
