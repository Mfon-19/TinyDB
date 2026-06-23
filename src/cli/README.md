# src/cli — Interactive shell (REPL)

The top-level entry point a person actually runs: open a database file, read SQL
at a prompt, execute it via the engine, and print results as a table. This is the
thinnest layer — all real work happens below it.

## Responsibilities
- Parse process arguments (database file path, optional script to run).
- Run a read-eval-print loop: read a statement, hand it to the engine, print the
  `ResultSet` (or an error) as a formatted grid.
- Handle meta/dot-commands (e.g. `.tables`, `.schema`, `.exit`).
- Translate engine `Status`/errors into friendly messages (exceptions are caught
  here, at the edge — internal layers use return-status, not throws).

## Planned files
- `main.cpp` — argument parsing, open the database, start the loop.
- `repl.h` / `repl.cpp` — the read-eval-print loop and statement buffering
  (accumulate input until a terminating `;`).
- `table_printer.h` / `table_printer.cpp` — render a `ResultSet` as aligned
  columns with a header.

## Key decisions
- CLI depends on the public API in `include/synarch`, not on internal headers.
- Keep it dumb: no SQL logic here, just I/O and formatting.
