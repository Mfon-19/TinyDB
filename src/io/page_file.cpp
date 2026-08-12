#include "io/page_file.h"

#include "io/file_io.h"
#include "io/testable_posix.h"

#include <fcntl.h>

#include <algorithm>
#include <concepts>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

namespace tinydb::io {
namespace {

auto OpenBuffered(const std::filesystem::path &path) -> Result<UniqueFd> {
  auto descriptor = UniqueFd(Open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644));
  if (!descriptor.Valid()) {
    return std::unexpected(ErrnoStatus("open database"));
  }
  return descriptor;
}

auto OpenDirect(const std::filesystem::path &path) -> Result<DirectFile> {
  // A direct request never opens a buffered descriptor. Every open and create
  // attempt uses O_DIRECT and validates the alignment.
  auto opened = DirectFile::OpenExistingIfPresent(path, O_RDWR);
  if (!opened) {
    return std::unexpected(opened.error());
  }
  if (opened->has_value()) {
    // Existing files can retain pages from prior buffered use. Ask the kernel
    // to discard that residency before direct traffic starts.
    opened->value().DiscardBufferedResidency();  // NOLINT(bugprone-unchecked-optional-access)
    return std::move(opened->value());           // NOLINT(bugprone-unchecked-optional-access)
  }

  auto created = DirectFile::CreateExclusiveIfAbsent(path, O_RDWR, 0644);
  if (!created) {
    return std::unexpected(created.error());
  }
  if (created->has_value()) {
    return std::move(created->value());  // NOLINT(bugprone-unchecked-optional-access)
  }

  // One racing create is harmless. This code reopens the winner once and does
  // not spin through repeated namespace changes.
  auto retry = DirectFile::OpenExisting(path, O_RDWR);
  if (!retry) {
    return std::unexpected(retry.error());
  }
  retry->DiscardBufferedResidency();
  return retry;
}

}  // namespace

auto PageFile::Open(const std::filesystem::path &path, PageIoMode mode) -> Result<PageFile> {
  // Construction selects one variant alternative. Later operations cannot
  // change the transport.
  switch (mode) {
    case PageIoMode::Buffered: {
      auto buffered = OpenBuffered(path);
      if (!buffered) {
        return std::unexpected(buffered.error());
      }
      return PageFile(Transport{std::move(*buffered)});
    }
    case PageIoMode::Direct: {
      auto direct = OpenDirect(path);
      if (!direct) {
        return std::unexpected(direct.error());
      }
      return PageFile(Transport{std::move(*direct)});
    }
  }
  return std::unexpected(Status::InvalidArgument("unknown database page I/O mode"));
}

auto PageFile::ReadPage(page_id_t page_id, std::span<std::byte, PAGE_SIZE> page) const -> Status {
  return std::visit(
      [&](const auto &transport) -> Status {
        using Backend = std::decay_t<decltype(transport)>;
        if constexpr (std::same_as<Backend, UniqueFd>) {
          const auto read = FullPread(transport.Get(), page.data(), page.size(), page_id * PAGE_SIZE);
          if (!read) {
            return read.error();
          }
          if (*read != PAGE_SIZE) {
            // Both transports require one complete persistent page. An EOF
            // inside that page is corruption, not a successful short read.
            return Status::Corruption("short read on a persistent page");
          }
          return {};
        } else {
          return transport.ReadPage(page_id, page);
        }
      },
      transport_);
}

auto PageFile::WritePage(page_id_t page_id, std::span<const std::byte, PAGE_SIZE> page) const -> Status {
  return std::visit(
      [&](const auto &transport) -> Status {
        using Backend = std::decay_t<decltype(transport)>;
        if constexpr (std::same_as<Backend, UniqueFd>) {
          return FullPwrite(transport.Get(), page.data(), page.size(), page_id * PAGE_SIZE);
        } else {
          return transport.WritePage(page_id, page);
        }
      },
      transport_);
}

auto PageFile::WritePages(page_id_t first_page_id, std::span<const std::byte *const> pages) const -> Status {
  return std::visit(
      [&](const auto &transport) -> Status {
        using Backend = std::decay_t<decltype(transport)>;
        if constexpr (std::same_as<Backend, UniqueFd>) {
          if (pages.empty() || pages.size() > MAX_PAGE_WRITE_BATCH_PAGES ||
              pages.size() - 1U > std::numeric_limits<page_id_t>::max() - first_page_id ||
              std::ranges::any_of(pages, [](const auto *page) { return page == nullptr; })) {
            return Status::InvalidArgument("database page write batch has an invalid layout");
          }
          constexpr auto maximum_page_id =
              (static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - (PAGE_SIZE - 1U)) / PAGE_SIZE;
          if (first_page_id > maximum_page_id || pages.size() - 1U > maximum_page_id - first_page_id) {
            return Status::InvalidArgument("database page write batch exceeds off_t");
          }
          // Buffered mode preserves the physical page order. Direct mode can
          // submit the same logical batch as one vectored run.
          for (auto index = std::size_t{0}; index < pages.size(); ++index) {
            if (auto status =
                    FullPwrite(transport.Get(), pages[index], PAGE_SIZE, (first_page_id + index) * PAGE_SIZE);
                !status.Ok()) {
              return status;
            }
          }
          return {};
        } else {
          // The strict direct transport validates its own batch shape, range,
          // and alignment; a second copy of those checks here would drift.
          return transport.WritePages(first_page_id, pages);
        }
      },
      transport_);
}

auto PageFile::IsDirect() const noexcept -> bool { return std::holds_alternative<DirectFile>(transport_); }

auto PageFile::BeginDirectReadPages(std::span<const page_id_t> page_ids,
                                    std::span<std::byte> contiguous_pages) const -> Result<DirectReadRequest> {
  const auto *const direct = std::get_if<DirectFile>(&transport_);
  if (direct == nullptr) {
    return std::unexpected(Status::InvalidArgument("asynchronous direct read requires direct page I/O"));
  }
  return direct->BeginReadPages(page_ids, contiguous_pages);
}

auto PageFile::BeginDirectWritePages(page_id_t first_page_id,
                                     std::span<const struct iovec> vectors) const -> Result<DirectWriteRequest> {
  const auto *const direct = std::get_if<DirectFile>(&transport_);
  if (direct == nullptr) {
    return std::unexpected(Status::InvalidArgument("asynchronous direct write requires direct page I/O"));
  }
  return direct->BeginWritePages(first_page_id, vectors);
}

auto PageFile::EnsurePageCount(page_id_t page_count) const -> Status {
  constexpr auto maximum_pages = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) / PAGE_SIZE;
  if (page_count > maximum_pages) {
    return Status::ResourceExhausted("database page count exceeds the platform file limit");
  }
  return std::visit(
      [&](const auto &transport) -> Status {
        using Backend = std::decay_t<decltype(transport)>;
        if constexpr (std::same_as<Backend, UniqueFd>) {
          if (Ftruncate(transport.Get(), page_count * PAGE_SIZE) < 0) {
            return ErrnoStatus("resize database");
          }
          return {};
        } else {
          return transport.EnsurePageCount(page_count);
        }
      },
      transport_);
}

auto PageFile::Stat(struct stat *result) const -> Status {
  if (result == nullptr) {
    return Status::InvalidArgument("database file stat requires an output buffer");
  }
  return std::visit(
      [&](const auto &transport) -> Status {
        using Backend = std::decay_t<decltype(transport)>;
        if constexpr (std::same_as<Backend, UniqueFd>) {
          if (Fstat(transport.Get(), result) < 0) {
            return ErrnoStatus("fstat database");
          }
          return {};
        } else {
          return transport.Stat(result);
        }
      },
      transport_);
}

auto PageFile::Sync() const -> Status {
  return std::visit(
      [&](const auto &transport) -> Status {
        using Backend = std::decay_t<decltype(transport)>;
        if constexpr (std::same_as<Backend, UniqueFd>) {
          if (Fsync(transport.Get()) < 0) {
            return ErrnoStatus("fsync database");
          }
          return {};
        } else {
          return transport.Sync();
        }
      },
      transport_);
}

}  // namespace tinydb::io
