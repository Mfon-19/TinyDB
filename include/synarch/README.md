# include/synarch — Public API headers

The stable, public-facing interface to the engine. Anything that wants to embed
synarch (the CLI, the test suite, or a future application) includes **only**
these headers. Internal headers under `src/**` are implementation detail and are
not installed.

## What lives here
- `database.h` — `Database`: `open(path)`, `close()`, `execute(sql) -> ResultSet`.
  The single entry point for embedders.
- `result.h` — `ResultSet` (column names + rows) and `Value` accessors for
  reading query output.
- `status.h` — `Status` / `Result<T>` return types and error codes used across
  the public boundary.

## Key decisions
- One namespace, `synarch`, for all public types.
- The public surface stays small and storage-agnostic, so internals (page format,
  B+-tree, executor) can change without breaking embedders.
- Header layout mirrors how the library is installed: `#include <synarch/database.h>`.
