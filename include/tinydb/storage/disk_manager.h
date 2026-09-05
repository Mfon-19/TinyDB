#pragma once

/*
 * This is the interface with Linux that handles the initial
 * creation of the database file and subsequent reading and
 * writing to the file. The open descriptor holds an exclusive
 * process lock.
 */

#include "tinydb/detail/file.h"
#include "tinydb/status.h"
#include "tinydb/storage/page.h"
#include <string_view>
#include <utility>

namespace tinydb::storage {

class DiskManager {
public:
  [[nodiscard]] static auto Open(std::string_view name) -> Result<DiskManager>;

  DiskManager(const DiskManager &) = delete;
  auto operator=(const DiskManager &) -> DiskManager & = delete;

  DiskManager(DiskManager &&other) noexcept = default;
  auto operator=(DiskManager &&other) noexcept -> DiskManager & = default;

  [[nodiscard]] auto PageCount() const -> Result<PageId>;

  auto WritePage(PageId page_id, const PageBytes &page) -> Status;
  auto ReadPage(PageId page_id, PageBytes &page) const -> Status;
  auto Sync() const -> Status;

private:
  explicit DiskManager(detail::File file) noexcept : file_(std::move(file)) {}
  detail::File file_;
};

auto SyncParentDirectory(std::string_view path) -> Status;
} // namespace tinydb::storage
