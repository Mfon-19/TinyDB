# TinyDB command-line interface

The CLI is a thin one-command-per-process client of the installed TinyDB API.
It opens the database, performs one operation, reports any `Status`, and closes
the handle before exiting. It contains no storage logic and is not an
interactive shell.

```text
tinydb [--direct-io] <database> put <key> <value>
tinydb [--direct-io] <database> get <key>
tinydb [--direct-io] <database> del <key>
tinydb [--direct-io] <database> scan
tinydb [--direct-io] <database> scan <lower> <upper>
```

The command uses buffered page I/O by default. The `--direct-io` option must
occur before the database path.

`scan` streams rows from a `ReadTransaction` cursor. Two bounds describe the
half-open range `[lower, upper)`; no bounds scan the complete keyspace. Keys
and values are written separated by a tab.

Exit status is 0 on success, 1 for a database error or missing key, and 2 for
invalid command syntax. Because commits are durable before they return, the
explicit `Close` is resource cleanup and an opportunity to report checkpoint
failure rather than the mutation durability boundary.

One binary-safe companion program is installed with the CLI:

```text
tinydb_dump [--direct-io] <database>
```

`tinydb_dump` performs normal recovery and full verification, then writes one
row per line as hexadecimal key, tab, and hexadecimal value. The encoding is
reversible for arbitrary byte strings.
