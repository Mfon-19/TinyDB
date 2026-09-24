#pragma once

/*
 * A SQLite virtual table module that stores tables in a TinyDB database:
 *
 *   CREATE VIRTUAL TABLE users USING tinydb('data.tinydb',
 *                                          id INTEGER PRIMARY KEY, name TEXT);
 *
 * SQLite keeps the schema; TinyDB holds the rows, keyed by the table name and
 * the encoded primary key. Tables in one TinyDB file share its transaction.
 * Each TinyDB file may be used by one SQLite connection at a time.
 */

#include <sqlite3.h>

extern "C" int tinydb_sqlite_register(sqlite3 *db);
