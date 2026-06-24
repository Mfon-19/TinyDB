# src/cli — Interactive shell (REPL)

The thin command-line front-end people actually run: open a database file and
drive the storage engine's key-value API from a prompt. All real work happens
below it in `src/engine`; this layer is just I/O and formatting, and exists mostly
to demo and poke at the engine by hand.

## Commands (v1)
```
put <key> <value>      insert or overwrite
get <key>              print the value (or "not found")
del <key>              delete
scan <lo> <hi>         print ordered key/value pairs in range
.stats                 buffer-pool hit rate, page count, WAL size
.exit
```

## Responsibilities
- Parse arguments (database file path, optional script to run).
- Read commands, call the matching `StorageEngine` method, print the result.
- Surface engine `Status` errors as friendly messages (exceptions, if any, are
  caught here at the edge — internal layers return status, per `docs/STYLE.md`).

## Planned files
- `main.cpp` — argument parsing, open the engine, start the loop.
- `repl.h` / `repl.cpp` — the read-eval-print loop and command dispatch.

## Key decisions
- Depends only on the public API in `include/synarch`, not internal headers.
- No storage logic here — purely a driver for the engine.
- The `.stats` command surfaces the metrics that matter for `bench/`.
