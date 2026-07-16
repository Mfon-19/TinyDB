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
