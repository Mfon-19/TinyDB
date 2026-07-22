#pragma once

#include <cstdint>
#include <filesystem>

namespace tinydb::bench {

/*
** Linux accounts buffered database pages in the file page cache, outside the
** process RSS.  mincore(2) lets the benchmark observe that otherwise hidden
** half of a double-buffered design without reading and thereby faulting the
** file itself.
*/
struct FileResidency final {
  std::uint64_t file_bytes{0};
  std::uint64_t pages{0};
  std::uint64_t resident_pages{0};
  std::uint64_t resident_bytes{0};

  auto Ratio() const -> double;
};

/*
** /proc/self/io separates bytes requested through the syscall interface from
** bytes Linux actually submitted to storage.  The former exposes read shape;
** the latter exposes page-cache hits, readahead, and writeback effects.
*/
struct ProcessIo final {
  std::uint64_t characters_read{0};
  std::uint64_t characters_written{0};
  std::uint64_t read_syscalls{0};
  std::uint64_t write_syscalls{0};
  std::uint64_t storage_read_bytes{0};
  std::uint64_t storage_write_bytes{0};
  std::uint64_t cancelled_write_bytes{0};
};

struct ProcessUsage final {
  double user_seconds{0};
  double system_seconds{0};
  std::uint64_t minor_faults{0};
  std::uint64_t major_faults{0};
  std::uint64_t voluntary_context_switches{0};
  std::uint64_t involuntary_context_switches{0};
};

struct ProcessMemory final {
  std::uint64_t resident_bytes{0};
  std::uint64_t proportional_bytes{0};
};

auto ObserveFileResidency(const std::filesystem::path &path) -> FileResidency;

/*
** POSIX_FADV_DONTNEED is advisory.  A true result says Linux accepted the
** request, not that every page disappeared.  Callers must record a residency
** observation afterwards; that observation is the actual precondition.
*/
auto AdviseDropFileCache(const std::filesystem::path &path) -> bool;

auto ObserveProcessIo() -> ProcessIo;
auto SubtractProcessIo(const ProcessIo &after, const ProcessIo &before) -> ProcessIo;
auto ObserveProcessUsage() -> ProcessUsage;
auto SubtractProcessUsage(const ProcessUsage &after, const ProcessUsage &before) -> ProcessUsage;
auto ObserveProcessMemory() -> ProcessMemory;
void WarmDatabaseFamily(const std::filesystem::path &database);

}  // namespace tinydb::bench
