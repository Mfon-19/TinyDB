# src/exec — Planner, operators & expression evaluation

Turns a bound statement (`src/sql`) into a tree of operators and runs it. Uses
the **Volcano / iterator model**: every operator exposes `next()` and pulls rows
from its child, so memory stays bounded and operators compose freely.

## Responsibilities
- **Plan**: map a bound statement to an operator tree (v1 plans are simple and
  mostly structural; a cost-based optimizer is a later stretch).
- **Execute**: drive the operator tree, producing result rows or applying writes.
- **Evaluate expressions**: compute `WHERE` predicates and projection values over
  a row, using the comparison/type rules from `src/record`.

## Operators (v1)
- `CreateTable` — allocate a B+-tree, register it in the catalog.
- `Insert` — encode a row (`src/record`) and insert via a B+-tree cursor.
- `SeqScan` — iterate a table's B+-tree leaf-to-leaf via a cursor.
- `Filter` — drop rows failing a `WHERE` predicate.
- `Project` — compute output columns / expressions.
- `Sort` — materialize and order rows for `ORDER BY`.

## Planned files
- `planner.h` / `planner.cpp` — bound statement → operator tree.
- `operator.h` — `Operator` base interface: `open()`, `next()`, `close()`.
- `operators.cpp` — the v1 operators above.
- `expr_eval.h` / `expr_eval.cpp` — evaluate a bound `Expr` against a row.
- `executor.h` / `executor.cpp` — top-level `execute(stmt) -> ResultSet`.

## Key decisions
- Iterator model first; a bytecode VM (VDBE-style) is a documented stretch goal.
- Operators depend only on the B+-tree cursor + record codec, never on raw pages.
