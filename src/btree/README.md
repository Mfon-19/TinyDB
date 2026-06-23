# src/btree — B+-tree

The indexing layer, built on top of the pager. Implements a disk-resident
**B+-tree** keyed by an integer `rowid`. Table data lives in these trees: each
leaf cell holds one serialized row (see `src/record`). Higher layers iterate and
mutate tables exclusively through the **cursor** API — they never read raw nodes.

## Responsibilities
- Lay out interior and leaf nodes inside 4 KB pages (cell pointer array + cells).
- Search by key (binary search within a node, descend the tree).
- Insert, update, and delete cells.
- **Split** full nodes and **merge/rebalance** underfull ones, propagating up.
- Store rows larger than a page on **overflow page** chains.
- Provide a stateful **cursor** for seek / next / prev / insert / delete.

## Planned files
- `node.h` / `node.cpp` — page-level node format: header, cell pointer array,
  cell parsing, free-space tracking.
- `btree.h` / `btree.cpp` — `BTree` class: `create()`, `search()`, `insert()`,
  `remove()`, root tracking; split/merge logic.
- `cursor.h` / `cursor.cpp` — `Cursor`: `seek(key)`, `first()`, `next()`,
  `key()`, `payload()`, `insert()`, `erase()`.
- `overflow.h` / `overflow.cpp` — write/read payloads that spill across pages.

## Key decisions
- B+-tree (not B-tree): all rows in leaves, leaves linked for fast scans.
- Integer `rowid` keys for v1; variable-length keys (for secondary indexes)
  are a later extension.
- Cell layout modeled on SQLite's for compactness and proven correctness.
