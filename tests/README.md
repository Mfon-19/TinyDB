# tests — Test suite

Where correctness is enforced. A storage engine is pointer- and byte-layout-heavy
and persists state across runs, so testing is first-class, not an afterthought.
Three kinds of tests live here.

## 1. Unit tests (per layer)
One test file per component, mirroring `src/`:
- `pager_test.cpp` — page read/write round-trips, cache eviction, freelist reuse.
- `btree_test.cpp` — insert/search/delete, node splits & merges, overflow pages,
  large randomized insert/lookup sequences.
- `record_test.cpp` — value (de)serialization round-trips, comparison rules.
- `catalog_test.cpp` — create/drop/lookup, schema reload after reopen.
- `binder_test.cpp` — name resolution, `*` expansion, type-error detection.
- `exec_test.cpp` — each operator and expression evaluation.

## 2. Golden SQL tests
`sql/` — `.sql` input files paired with expected-output files. A runner executes
the SQL against a fresh database and diffs the printed result. This is the main
end-to-end safety net for query behavior.

## 3. Crash-recovery tests
`recovery/` — open a database, begin a write, simulate a crash (abort before
sync / kill the process), reopen, and assert the database is consistent and the
incomplete write left no corruption. Validates the durability layer.

## Tooling
- Framework: GoogleTest (decided in `docs/DESIGN.md`).
- Debug builds run under **ASan/UBSan**; the randomized B+-tree tests are the
  highest-value bug finders.
