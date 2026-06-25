# include/tinydb — Public API headers

The stable, public-facing interface to the storage engine. Anything that embeds
tinydb — the CLI, the test suite, the benchmarks, or a future application —
includes **only** these headers. Internal headers under `src/**` are
implementation detail and are not installed.

## What lives here
- `storage_engine.h` — `StorageEngine`: `Open(path, options)`, `Put`, `Get`,
  `Remove`, `Scan`, `Close`. The single entry point for embedders.
- `slice.h` — `Slice`: the non-owning byte-string view used for keys and values.
- `status.h` — `Status` / `Result<T>` return types and error codes used across
  the public boundary.
- `options.h` — open-time configuration (buffer-pool size, sync mode, path).

## Key decisions
- One namespace, `tinydb`, for all public types.
- The public surface is a **byte-string key-value store** — small, stable, and
  storage-agnostic, so internals (page format, buffer pool, B+-tree, WAL) can
  change without breaking embedders.
- Header layout mirrors how the library installs: `#include <tinydb/storage_engine.h>`.
