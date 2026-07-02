#pragma once

#include <cstdio>
#include <cstdlib>

namespace tinydb {

[[noreturn]] inline void PanicAt(const char *file, int line, const char *message) {
  std::fprintf(stderr, "tinydb: invariant violated at %s:%d: %s\n", file, line, message);
  std::abort();
}

}  // namespace tinydb

// Always-on invariant check; a violated invariant means the caller's state is
// broken and continuing would corrupt data, so abort loudly
#define TINYDB_CHECK(condition, message)                \
  do {                                                  \
    if (!(condition)) {                                 \
      ::tinydb::PanicAt(__FILE__, __LINE__, (message)); \
    }                                                   \
  } while (false)
