# src/storage — Pager & file format

The lowest layer. Owns the single database file and turns it into a sequence of
fixed-size pages with an in-memory cache. Everything above this layer (B+-tree,
catalog, executor) reads and writes **only** through the pager — nothing else
touches the file directly.

## Responsibilities
- Open/create the database file and validate/initialize its header.
- Read and write fixed-size **4 KB pages** by page number.
- Maintain an in-memory **buffer pool** (page cache) with pin/unpin and eviction.
- Track and recycle free pages via a **freelist**.
- Hand pages to the durability layer before they're flushed (see `docs/DESIGN.md`).

## File format (v1)
- **Page 0 — header**: magic string, file format version, page size, total page
  count, freelist head page number, catalog (schema) root page number.
- **Pages 1..N**: B+-tree nodes, overflow pages, and free pages.

## Planned files
- `pager.h` / `pager.cpp` — `Pager` class: `get_page(pgno)`, `make_page()`,
  `mark_dirty()`, `free_page()`, `flush()`, `sync()`.
- `page.h` — `Page` struct: page number, raw 4 KB buffer, dirty/pin flags.
- `page_cache.h` / `page_cache.cpp` — buffer pool + eviction policy (start with
  clock/LRU).
- `file_header.h` / `file_header.cpp` — read/write/validate the page-0 header.
- `freelist.h` / `freelist.cpp` — allocate/release free pages.

## Key decisions
- Page size fixed at 4 KB for v1 (stored in the header so it can change later).
- Single-threaded access for v1; locking is added with transactions.
