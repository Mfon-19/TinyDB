#pragma once

#include <cstdio>
#include <cstdlib>

namespace tinydb {

[[noreturn]] inline void PanicAt(const char *file, int line, const char *message) {
  std::fprintf(stderr, "tinydb: invariant violated at %s:%d: %s\n", file, line, message);
  std::abort();
}

}  // namespace tinydb

/*
** TINYDB_CHECK is for internal invariants, not errors caused by untrusted
** input. Continuing after one of these checks fails could corrupt data, so the
** process reports the source location and aborts.
*/
#define TINYDB_CHECK(condition, message)                \
  do {                                                  \
    if (!(condition)) {                                 \
      ::tinydb::PanicAt(__FILE__, __LINE__, (message)); \
    }                                                   \
  } while (false)
