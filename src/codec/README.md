# src/codec — Encoding primitives

The low-level byte-encoding toolkit shared across the engine. Defines how keys,
values, and B+-tree cells are serialized into and parsed out of page buffers, plus
the primitives (varints, byte views, comparison) they're built from. Pure,
allocation-light, heavily unit- and fuzz-tested — corruption bugs hide here.

## Responsibilities
- Represent a borrowed byte range (`Slice`) without copying.
- Encode/decode **variable-length integers** (big-endian base-128) for compact
  lengths and page numbers.
- Serialize/parse a **B+-tree cell** — `(key, value)` in a leaf, `(key, child
  pgno)` in an interior node.
- Define **key comparison** (lexicographic byte order) — the ordering the whole
  index depends on.

## Planned files
- `slice.h` — `Slice`: a non-owning `(pointer, length)` view over bytes
  (`std::span<const std::byte>`-backed). The currency type for keys/values.
- `varint.h` / `varint.cpp` — `put_varint` / `get_varint`, 1–9 bytes.
- `cell.h` / `cell.cpp` — encode/decode leaf and interior cells.
- `compare.h` — lexicographic key comparison helpers.

## Key decisions
- Keys and values are **opaque byte strings** — this is a KV engine, not a typed
  relational store; meaning is the caller's concern.
- `Slice` is **non-owning** by design (see the ownership rule in `docs/STYLE.md`)
  — it borrows; it never frees.
- A prime **libFuzzer** target: feed random bytes to the cell/varint decoders and
  assert no crash, no UB, no over-read.
