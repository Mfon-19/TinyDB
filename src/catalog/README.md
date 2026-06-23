# src/catalog — Schema & system catalog

Tracks what tables exist and what their columns are. Persisted **inside the
database itself** as a reserved B+-tree (its root page number is recorded in the
file header), then cached in memory on open. This is what `CREATE TABLE` writes
to and what the binder (`src/sql`) reads to resolve names and types.

## Responsibilities
- Maintain the system catalog: one row per user table.
- Persist a table definition on `CREATE TABLE`; remove it on `DROP TABLE`.
- Load and cache all schema into memory when the database is opened.
- Answer lookups: does table T exist? what is its root page and column list?

## Catalog row (per table)
- table name
- root page number of the table's B+-tree
- column definitions: name, declared type, ordinal, NOT NULL / PRIMARY KEY flags
- (stored as a serialized record, like any other row)

## Planned files
- `catalog.h` / `catalog.cpp` — `Catalog`: `create_table()`, `drop_table()`,
  `lookup(name)`, `load()`; owns the in-memory map and the schema B+-tree.
- `table_schema.h` / `table_schema.cpp` — `TableSchema`, `ColumnDef`, and
  (de)serialization of a table definition to/from a catalog record.

## Key decisions
- Catalog is "just another B+-tree" — reuses the storage/record layers rather
  than inventing a separate format (the SQLite `sqlite_master` approach).
- In-memory cache is the source of truth at runtime; the tree is the durable copy.
