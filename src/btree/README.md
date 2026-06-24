# src/btree — B+-tree index

The ordered index and the core data structure of the engine. A disk-resident
**B+-tree** mapping **byte-string keys → byte-string values**, built on top of the
buffer pool (it fetches/pins pages, never touches the file directly). This is what
gives the engine `Get`, ordered `Scan`, and efficient `Put`/`Delete`.

## Responsibilities
- Lay out interior and leaf nodes inside 4 KB pages (slotted-page format).
- Search by key (binary search within a node, descend the tree).
- `Insert` / `Update` / `Delete` entries.
- **Split** full nodes and propagate splits toward the root.
- Link leaf nodes left-to-right so **range scans** are sequential.
- Provide a **cursor** for `Seek` / `Next` and range iteration.
- Emit WAL records for every structural change (see `src/wal`) so changes are
  recoverable.

## Layout
- **Leaf node**: sorted (key, value) cells + a pointer to the next leaf (for
  scans). Cells encoded via `src/codec`.
- **Interior node**: sorted separator keys + child page pointers.
- Keys/values larger than a page use overflow pages — **deferred**; v1 caps an
  entry to fit a page and errors otherwise.

## Planned files
- `node.h` / `node.cpp` — slotted-page node format: header, cell-pointer array,
  cell access, in-page free space.
- `btree.h` / `btree.cpp` — `BTree`: `Get`, `Put`, `Erase`, `Scan`; split logic
  and root tracking.
- `cursor.h` / `cursor.cpp` — `Cursor`: `Seek(key)`, `Next()`, `Key()`,
  `Value()` — drives range scans.

## Key decisions
- **B+-tree** (all values in leaves, leaves linked) — built for page-based,
  range-scan-friendly storage; the deliberate alternative to an LSM-tree.
- **Deletes use tombstones; node merging/rebalancing is deferred** — delete-merge
  is the fiddliest tree code and isn't needed for a correct, impressive v1.
- Correctness is enforced by **differential testing against `std::map`**
  (see `tests/`), the technique real engines use to find B+-tree bugs.
