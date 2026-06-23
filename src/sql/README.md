# src/sql — Parser, AST & binder

The SQL frontend: turn a SQL string into a validated, name-resolved plan input.
Two stages live here — **parsing** (text → AST) and **binding** (AST → resolved,
type-checked form against the catalog).

> The parser is the one component we do **not** write from scratch. We integrate
> an existing SQL parser behind a thin internal AST interface, so it can be
> swapped later without touching the binder or executor. Parser choice (e.g.
> hyrise/sql-parser vs. a Flex+Bison grammar) is recorded in `docs/DESIGN.md`.

## Responsibilities
- **Parse**: SQL text → internal AST (statements, expressions, clauses).
- Normalize the third-party parser's output into our own stable AST types.
- **Bind**: resolve table/column names against `src/catalog`, expand `SELECT *`,
  assign column indices, and type-check expressions.
- Produce the structure the planner/executor (`src/exec`) consumes.

## Statements (v1, Core SQL)
`CREATE TABLE`, `INSERT`, `SELECT` with `WHERE` and `ORDER BY`, single-table.

## Planned files
- `ast.h` — internal AST node types (`SelectStmt`, `InsertStmt`,
  `CreateTableStmt`, `Expr`, ...).
- `parser.h` / `parser.cpp` — wrapper over the chosen parser; emits our AST.
- `binder.h` / `binder.cpp` — `Binder`: name resolution, `*` expansion,
  type checking; outputs a bound statement.
- `bound.h` — bound/resolved statement types passed to the executor.

## Key decisions
- A stable internal AST insulates the rest of the engine from the parser choice.
- Binding is separate from execution so errors (unknown column, type mismatch)
  surface before any data is touched.
