#pragma once

#include <tinydb/options.h>
#include <tinydb/status.h>

#include "io/direct_file.h"
#include "storage/page.h"

#include <sys/stat.h>

#include <filesystem>
#include <memory>
#include <span>

namespace tinydb::io {

inline constexpr std::size_t MAX_PAGE_WRITE_BATCH_PAGES = MAX_DIRECT_WRITE_BATCH_PAGES;

/*
** DATABASE PAGE-FILE TRANSPORT
**
** When the database opens, PageFile selects one physical transport. Buffered
** mode uses ordinary pread and pwrite. Direct mode uses fail-closed O_DIRECT
** transfers. Direct mode never changes to buffered mode after an open error.
** The selected backend remains fixed for the PageFile lifetime. Both modes
** expose the same fixed-page and durability operations to recovery and
** DiskManager. The transport never selects a persistent format or WAL
** protocol.
*/
class PageFile final {
 public:
  static auto Open(const std::filesystem::path &path, PageIoMode mode) -> Result<PageFile>;

  PageFile(const PageFile &) = delete;
  auto operator=(const PageFile &) -> PageFile & = delete;
  PageFile(PageFile &&) noexcept;
  auto operator=(PageFile &&) noexcept -> PageFile &;
  ~PageFile();

  auto ReadPage(page_id_t page_id, std::span<std::byte, PAGE_SIZE> page) const -> Status;
  auto WritePage(page_id_t page_id, std::span<const std::byte, PAGE_SIZE> page) const -> Status;
  auto WritePages(page_id_t first_page_id, std::span<const std::byte *const> pages) const -> Status;
  auto IsDirect() const noexcept -> bool;
  // Asynchronous preparation requires DirectFile as the selected backend.
  auto BeginDirectReadPages(std::span<const page_id_t> page_ids,
                            std::span<std::byte> contiguous_pages) const -> Result<DirectReadRequest>;
  auto BeginDirectWritePages(page_id_t first_page_id,
                             std::span<const struct iovec> vectors) const -> Result<DirectWriteRequest>;
  auto EnsurePageCount(page_id_t page_count) const -> Status;
  auto Stat(struct stat *result) const -> Status;
  auto Sync() const -> Status;

 private:
  struct Impl;
  explicit PageFile(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace tinydb::io
