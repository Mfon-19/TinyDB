#include "system_metrics.h"

#include "benchmark.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace tinydb::bench {
namespace {

auto ParseCounter(std::string_view text, std::string_view name) -> std::uint64_t {
  const auto separator = text.find(':');
  if (separator == std::string_view::npos) {
    Fail("malformed /proc/self/io counter");
  }
  auto value_text = text.substr(separator + 1U);
  const auto first = value_text.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    Fail("empty /proc/self/io counter");
  }
  value_text.remove_prefix(first);
  auto value = std::uint64_t{0};
  const auto parsed = std::from_chars(value_text.data(), value_text.data() + value_text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != value_text.data() + value_text.size()) {
    Fail(std::string("invalid /proc/self/io counter: ") + std::string(name));
  }
  return value;
}

auto ParseKilobytes(std::string_view text, std::string_view name) -> std::uint64_t {
  const auto separator = text.find(':');
  if (separator == std::string_view::npos) {
    Fail("malformed memory counter");
  }
  auto value_text = text.substr(separator + 1U);
  const auto first = value_text.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    Fail("empty memory counter");
  }
  value_text.remove_prefix(first);
  auto value = std::uint64_t{0};
  const auto parsed = std::from_chars(value_text.data(), value_text.data() + value_text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr == value_text.data()) {
    Fail(std::string("invalid memory counter: ") + std::string(name));
  }
  return value;
}

auto CounterDelta(std::uint64_t after, std::uint64_t before) -> std::uint64_t {
  return after >= before ? after - before : 0;
}

auto Seconds(const timeval &value) -> double {
  return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1'000'000.0;
}

auto FamilyMembers(const std::filesystem::path &database) -> std::vector<std::filesystem::path> {
  auto members = std::vector<std::filesystem::path>{};
  const auto wal_name = database.filename().string() + "-wal";
  const auto archive_prefix = wal_name + '.';
  auto error = std::error_code{};
  for (const auto &entry : std::filesystem::directory_iterator(database.parent_path(), error)) {
    const auto name = entry.path().filename().string();
    if (name == database.filename() || name == wal_name ||
        (name.starts_with(archive_prefix) && name.ends_with(".segment"))) {
      members.push_back(entry.path());
    }
  }
  if (error || members.empty()) {
    Fail("cannot enumerate the database family");
  }
  return members;
}

}  // namespace

auto FileResidency::Ratio() const -> double {
  return pages == 0 ? 0.0 : static_cast<double>(resident_pages) / static_cast<double>(pages);
}

auto ObserveFileResidency(const std::filesystem::path &path) -> FileResidency {
  const auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    Fail("cannot open database file for a residency observation");
  }

  struct stat file {};
  if (::fstat(fd, &file) != 0 || file.st_size < 0) {
    ::close(fd);
    Fail("cannot stat database file for a residency observation");
  }
  if (file.st_size == 0) {
    ::close(fd);
    return {};
  }

  const auto page_size_raw = ::sysconf(_SC_PAGESIZE);
  if (page_size_raw <= 0) {
    ::close(fd);
    Fail("cannot determine the Linux page size");
  }
  const auto page_size = static_cast<std::uint64_t>(page_size_raw);
  const auto file_bytes = static_cast<std::uint64_t>(file.st_size);
  const auto pages = (file_bytes + page_size - 1U) / page_size;
  if (file_bytes > std::numeric_limits<std::size_t>::max() || pages > std::numeric_limits<std::size_t>::max()) {
    ::close(fd);
    Fail("database file is too large for a residency observation");
  }

  /*
  ** Mapping with PROT_NONE creates page-table metadata but does not read file
  ** contents.  mincore then reports residency that existed before the probe.
  */
  auto *mapping = ::mmap(nullptr, static_cast<std::size_t>(file_bytes), PROT_NONE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (mapping == MAP_FAILED) {
    Fail("cannot map database file for a residency observation");
  }
  auto vector = std::vector<unsigned char>(static_cast<std::size_t>(pages));
  if (::mincore(mapping, static_cast<std::size_t>(file_bytes), vector.data()) != 0) {
    ::munmap(mapping, static_cast<std::size_t>(file_bytes));
    Fail("mincore failed while observing database-file residency");
  }
  ::munmap(mapping, static_cast<std::size_t>(file_bytes));

  auto resident_pages = std::uint64_t{0};
  for (const auto state : vector) {
    resident_pages += (state & 1U) != 0 ? 1U : 0U;
  }
  return FileResidency{
      .file_bytes = file_bytes,
      .pages = pages,
      .resident_pages = resident_pages,
      .resident_bytes = resident_pages * page_size,
  };
}

auto AdviseDropFileCache(const std::filesystem::path &path) -> bool {
  const auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    Fail("cannot open database file for cache eviction advice");
  }
  const auto result = ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
  ::close(fd);
  return result == 0;
}

auto ObserveProcessIo() -> ProcessIo {
  auto input = std::ifstream{"/proc/self/io"};
  if (!input) {
    Fail("cannot open /proc/self/io; direct-I/O metrics require Linux procfs");
  }

  auto result = ProcessIo{};
  auto fields = std::uint32_t{0};
  auto line = std::string{};
  while (std::getline(input, line)) {
    const auto view = std::string_view{line};
    if (view.starts_with("rchar:")) {
      result.characters_read = ParseCounter(view, "rchar");
      fields |= 1U << 0U;
    } else if (view.starts_with("wchar:")) {
      result.characters_written = ParseCounter(view, "wchar");
      fields |= 1U << 1U;
    } else if (view.starts_with("syscr:")) {
      result.read_syscalls = ParseCounter(view, "syscr");
      fields |= 1U << 2U;
    } else if (view.starts_with("syscw:")) {
      result.write_syscalls = ParseCounter(view, "syscw");
      fields |= 1U << 3U;
    } else if (view.starts_with("read_bytes:")) {
      result.storage_read_bytes = ParseCounter(view, "read_bytes");
      fields |= 1U << 4U;
    } else if (view.starts_with("write_bytes:")) {
      result.storage_write_bytes = ParseCounter(view, "write_bytes");
      fields |= 1U << 5U;
    } else if (view.starts_with("cancelled_write_bytes:")) {
      result.cancelled_write_bytes = ParseCounter(view, "cancelled_write_bytes");
      fields |= 1U << 6U;
    }
  }
  if (fields != (1U << 7U) - 1U) {
    Fail("/proc/self/io did not expose every required counter");
  }
  return result;
}

auto SubtractProcessIo(const ProcessIo &after, const ProcessIo &before) -> ProcessIo {
  return ProcessIo{
      .characters_read = CounterDelta(after.characters_read, before.characters_read),
      .characters_written = CounterDelta(after.characters_written, before.characters_written),
      .read_syscalls = CounterDelta(after.read_syscalls, before.read_syscalls),
      .write_syscalls = CounterDelta(after.write_syscalls, before.write_syscalls),
      .storage_read_bytes = CounterDelta(after.storage_read_bytes, before.storage_read_bytes),
      .storage_write_bytes = CounterDelta(after.storage_write_bytes, before.storage_write_bytes),
      .cancelled_write_bytes = CounterDelta(after.cancelled_write_bytes, before.cancelled_write_bytes),
  };
}

auto ObserveProcessUsage() -> ProcessUsage {
  auto usage = rusage{};
  if (::getrusage(RUSAGE_SELF, &usage) != 0) {
    Fail("getrusage failed");
  }
  return ProcessUsage{
      .user_seconds = Seconds(usage.ru_utime),
      .system_seconds = Seconds(usage.ru_stime),
      .minor_faults = static_cast<std::uint64_t>(std::max<long>(0, usage.ru_minflt)),
      .major_faults = static_cast<std::uint64_t>(std::max<long>(0, usage.ru_majflt)),
      .voluntary_context_switches = static_cast<std::uint64_t>(std::max<long>(0, usage.ru_nvcsw)),
      .involuntary_context_switches = static_cast<std::uint64_t>(std::max<long>(0, usage.ru_nivcsw)),
  };
}

auto SubtractProcessUsage(const ProcessUsage &after, const ProcessUsage &before) -> ProcessUsage {
  return ProcessUsage{
      .user_seconds = std::max(0.0, after.user_seconds - before.user_seconds),
      .system_seconds = std::max(0.0, after.system_seconds - before.system_seconds),
      .minor_faults = CounterDelta(after.minor_faults, before.minor_faults),
      .major_faults = CounterDelta(after.major_faults, before.major_faults),
      .voluntary_context_switches =
          CounterDelta(after.voluntary_context_switches, before.voluntary_context_switches),
      .involuntary_context_switches =
          CounterDelta(after.involuntary_context_switches, before.involuntary_context_switches),
  };
}

auto ObserveProcessMemory() -> ProcessMemory {
  auto input = std::ifstream{"/proc/self/smaps_rollup"};
  if (!input) {
    Fail("cannot open /proc/self/smaps_rollup");
  }
  auto memory = ProcessMemory{};
  auto fields = std::uint32_t{0};
  auto line = std::string{};
  while (std::getline(input, line)) {
    const auto view = std::string_view{line};
    if (view.starts_with("Rss:")) {
      memory.resident_bytes = ParseKilobytes(view, "Rss") * 1'024U;
      fields |= 1U << 0U;
    } else if (view.starts_with("Pss:")) {
      memory.proportional_bytes = ParseKilobytes(view, "Pss") * 1'024U;
      fields |= 1U << 1U;
    }
  }
  if (fields != 3U) {
    Fail("/proc/self/smaps_rollup did not expose Rss and Pss");
  }
  return memory;
}

void WarmDatabaseFamily(const std::filesystem::path &database) {
  auto buffer = std::array<std::byte, 1U << 20U>{};
  for (const auto &path : FamilyMembers(database)) {
    const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
      Fail("cannot open database-family member for cache warming");
    }
    while (true) {
      const auto bytes = ::read(descriptor, buffer.data(), buffer.size());
      if (bytes == 0) {
        break;
      }
      if (bytes < 0) {
        ::close(descriptor);
        Fail("cannot warm database-family member");
      }
    }
    ::close(descriptor);
  }
}

}  // namespace tinydb::bench
