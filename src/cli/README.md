# TinyDB command-line interface

The CLI is a thin one-command-per-process client of the installed TinyDB API.
It opens the database, performs one operation, reports any `Status`, and closes
the handle before exiting. It contains no storage logic and is not an
interactive shell.

```text
tinydb <database> put <key> <value>
tinydb <database> get <key>
tinydb <database> del <key>
tinydb <database> scan
tinydb <database> scan <lower> <upper>
```

`scan` streams rows from a `ReadTransaction` cursor. Two bounds describe the
half-open range `[lower, upper)`; no bounds scan the complete keyspace. Keys
and values are written separated by a tab.

Exit status is 0 on success, 1 for a database error or missing key, and 2 for
invalid command syntax. Because commits are durable before they return, the
explicit `Close` is resource cleanup and an opportunity to report checkpoint
failure rather than the mutation durability boundary.

Two offline-oriented companion programs are installed with the CLI:

```text
tinydb_dump <database>
tinydb_salvage <damaged-source> <new-destination>
```

`tinydb_dump` performs normal recovery and full verification, then writes one
row per line as hexadecimal key, tab, and hexadecimal value. The encoding is
reversible for arbitrary byte strings.

`tinydb_salvage` is a distinct best-effort path, never part of normal open. It
holds a shared file lock, reads the source without modifying it, accepts
locally checksummed leaves even when both superblocks are damaged, and writes
surviving rows through the normal transaction path into a destination that
must not exist. Its report distinguishes damaged pages, damaged values,
duplicates, and whether allocator metadata was available to exclude known
obsolete pages. Recovered rows are not claimed to be an exact committed state
when global metadata is unavailable.
