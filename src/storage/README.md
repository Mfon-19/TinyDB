# src/storage — Pager, file format & VFS

The lowest layer. Owns the single database file, abstracts the OS I/O behind a
**VFS**, and turns the file into a sequence of fixed-size pages. Everything above
(WAL, buffer pool, B+-tree) reads and writes **only** through the pager — nothing
else touches the file directly.

## Responsibilities
- **VFS**: wrap OS file I/O (open, read, write, **durable sync**, truncate) so the
  rest of the engine is platform-agnostic. Durable flush is the one syscall that
  differs by OS (`fsync` on Linux, `F_FULLFSYNC` on macOS) — isolated here.
- Read and write fixed-size **4 KB pages** by page number.
- Allocate new pages and recycle freed ones (**freelist**).
- Maintain the file header (page 1) and keep it consistent on disk.

## File format (v1)
- Pages are **1-indexed**; page number `0` is the null sentinel ("no page").
- **Page 1 — header**: magic string, format version, page size, page count,
  B+-tree root page, freelist head + count, WAL state, change counter.
- **Pages 2..N**: B+-tree nodes, overflow pages (deferred), and free pages.
- Byte order: **big-endian** (portable, readable in hex dumps).
- Full spec lives in `docs/FILE_FORMAT.md`.

## Planned files
- `vfs.h` / `vfs.cpp` — `Vfs` interface: `read_at`, `write_at`, `sync`, `size`,
  `truncate`; a POSIX implementation.
- `pager.h` / `pager.cpp` — `Pager`: `read_page(pgno)`, `write_page(pgno)`,
  `allocate_page()`, `free_page(pgno)`, `sync()`.
- `page.h` — `Page`: page number + raw 4 KB buffer.
- `file_header.h` / `file_header.cpp` — read/write/validate page 1.
- `freelist.h` / `freelist.cpp` — simple linked-list free-page management.

## Key decisions
- Page size fixed at 4 KB for v1 (stored in the header so it can change later).
- Single database file; single writer / multiple readers for v1.
- The pager does raw page I/O; **caching lives one layer up** (`src/buffer`), and
  **durability ordering** (log-before-page) is enforced by `src/wal`.
