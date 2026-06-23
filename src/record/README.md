# src/record — Values & record codec

Defines the database's value types and the on-disk **record format** — how a row
of typed values is serialized into the compact byte payload stored in a B+-tree
leaf cell, and parsed back out.

## Responsibilities
- Represent a single SQL value (the `Value` variant) and its type tag.
- Serialize a row (vector of `Value`) → byte buffer.
- Deserialize a byte buffer → row, lazily decoding only the columns needed.
- Compare values (for `WHERE` and `ORDER BY`) with SQL semantics, incl. NULL.

## Types (v1)
`NULL`, `INTEGER` (64-bit), `REAL` (double), `TEXT` (UTF-8). `BLOB` is a trivial
later addition.

## Record format
Modeled on SQLite's record format: a header listing a **serial type** per column
(encoded as varints), followed by the column bodies. Serial types encode both the
type and, for text, the length — so NULLs and small integers cost almost nothing.

## Planned files
- `value.h` / `value.cpp` — `Value` (tagged union), type tag, comparison.
- `serial_type.h` — varint + serial-type encoding helpers.
- `record.h` / `record.cpp` — `encode_row()` / `decode_row()` and a
  `RecordReader` for lazy per-column access.
- `varint.h` / `varint.cpp` — variable-length integer encode/decode.

## Key decisions
- Lazy column decoding keeps scans with narrow projections cheap.
- Comparison rules centralized here so the executor stays simple.
