#pragma once

#include <tinydb/options.h>
#include <tinydb/status.h>

#include "io/direct_file.h"
#include "io/unique_fd.h"
#include "storage/page.h"

#include <sys/stat.h>

#include <filesystem>
#include <span>
#include <utility>
#include <variant>

namespace tinydb::io {

inline constexpr std::size_t MAX_PAGE_WRITE_BATCH_PAGES = MAX_DIRECT_WRITE_BATCH_PAGES;

/*
** DATABASE PAGE-FILE TRANSPORT
**
** PageFile chooses one database-page transport at open and keeps it for the
** lifetime of the object. The buffered backend uses ordinary pread and pwrite;
** the direct backend uses fail-closed O_DIRECT and never falls back after an
** open or transfer error. Both expose the same fixed-page and durability
** operations, and neither selects a persistent format or WAL protocol.
**
** Native request tokens borrow the DirectFile descriptor and path. The caller
** completes them before this object moves or is destroyed; DiskManager moves
** PageFile only during open, before a request can exist.
*/
class PageFile final {
 public:
  static auto Open(const std::filesystem::path &path, PageIoMode mode) -> Result<PageFile>;

  PageFile(const PageFile &) = delete;
  auto operator=(const PageFile &) -> PageFile & = delete;
  PageFile(PageFile &&) noexcept = default;
  auto operator=(PageFile &&) noexcept -> PageFile & = default;
  ~PageFile() = default;

  auto ReadPage(page_id_t page_id, std::span<std::byte, PAGE_SIZE> page) const -> Status;
  auto WritePage(page_id_t page_id, std::span<const std::byte, PAGE_SIZE> page) const -> Status;
  auto WritePages(page_id_t first_page_id, std::span<const std::byte *const> pages) const -> Status;
  auto IsDirect() const noexcept -> bool;
  auto BeginDirectReadPages(std::span<const page_id_t> page_ids,
                            std::span<std::byte> contiguous_pages) const -> Result<DirectReadRequest>;
  auto BeginDirectWritePages(page_id_t first_page_id,
                             std::span<const struct iovec> vectors) const -> Result<DirectWriteRequest>;
  auto EnsurePageCount(page_id_t page_count) const -> Status;
  auto Stat(struct stat *result) const -> Status;
  auto Sync() const -> Status;

 private:
  using Transport = std::variant<UniqueFd, DirectFile>;

  explicit PageFile(Transport transport) : transport_(std::move(transport)) {}

  Transport transport_;
};

}  // namespace tinydb::io
