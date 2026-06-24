# src/storage — Disk manager, page file & format

The lowest layer. Owns the single database file and turns it into fixed-size
pages. Everything above this layer (WAL ordering, buffer pool, B+-tree) talks in
page IDs and page buffers; only this layer owns the Linux file descriptor and raw
I/O syscalls.

The shape is intentionally small: a `DiskManager` boundary, not a plugin-style
file abstraction. A separate pager abstraction can be added later only if
header/free-list/file-format logic grows large enough to justify it.

## Responsibilities
- Open/close the database file with Linux file-descriptor APIs.
- Read and write fixed-size **4 KB pages** by page number using `pread`/`pwrite`.
- Provide durable flush with `fdatasync`/`fsync`; this is the only place that
  should know about Linux persistence syscalls.
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
- `disk_manager.h` / `disk_manager.cpp` — `DiskManager`: `Open`, `ReadPage`,
  `WritePage`, `AllocatePage`, `FreePage`, `Sync`, `Size`; owns the Linux fd.
- `page.h` — `Page`: page number + raw 4 KB buffer.
- `file_header.h` / `file_header.cpp` — read/write/validate page 1.
- `freelist.h` / `freelist.cpp` — simple linked-list free-page management.
- Optional later: `pager.h` / `pager.cpp` if page-file metadata needs a separate
  coordinator above `DiskManager`.

## Key decisions
- Page size fixed at 4 KB for v1 (stored in the header so it can change later).
- Linux is the storage target. macOS can remain a development machine, but
  durability-sensitive tests should run on Linux.
- Single database file; single writer / multiple readers for v1.
- `DiskManager` does raw page I/O; **caching lives one layer up** (`src/buffer`),
  and **durability ordering** (log-before-page) is enforced by `src/wal`.
