#include "salvage/salvage.h"

#include <tinydb/database.h>

#include "btree/page_format.h"
#include "btree/page_view.h"
#include "btree/value_storage.h"
#include "io/file_io.h"
#include "io/syscalls.h"
#include "io/unique_fd.h"
#include "txn/contract.h"
#include "txn/database_state.h"
#include "txn/transaction_pages.h"
#include "storage/superblock.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>

namespace tinydb::salvage {
namespace {

struct Candidate final {
  std::uint64_t page_lsn{0};
  std::string value;
};

using RecoveredRows = std::map<std::string, Candidate, txn::BytewiseLess>;

auto ReadPhysicalPage(int fd, page_id_t page_id) -> Result<std::shared_ptr<std::array<char, PAGE_SIZE>>> {
  auto page = std::make_shared<std::array<char, PAGE_SIZE>>();
  const auto bytes = io::FullPread(fd, page->data(), page->size(), page_id * PAGE_SIZE);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  if (*bytes != PAGE_SIZE) {
    return std::unexpected(Status::Corruption("physical page is truncated"));
  }
  return page;
}

/*
** RawPages validates only a page's local checksum and identity.  It does not
** require a usable superblock or root, which is the essential distinction
** between explicit salvage and a normal database open.
*/
class RawPages final : public PageReader {
 public:
  RawPages(UniqueFd fd, page_id_t page_count) : fd_(std::move(fd)), page_count_(page_count) {}

  auto Read(page_id_t page_id) -> Result<PageHandle> override {
    if (page_id < FIRST_DATA_PAGE_ID || page_id >= page_count_) {
      return std::unexpected(Status::Corruption("page reference is outside the physical source file"));
    }
    auto page = ReadPhysicalPage(fd_.Get(), page_id);
    if (!page) {
      return std::unexpected(page.error());
    }
    const auto header = storage::DecodeDataPageHeader(std::as_bytes(std::span{(*page)->data(), PAGE_SIZE}), page_id);
    if (!header) {
      return std::unexpected(header.error());
    }
    auto keeper = std::static_pointer_cast<const void>(*page);
    return PageHandle(page->get(), page_id, (*page)->data(), nullptr, std::move(keeper));
  }

 private:
  UniqueFd fd_;
  page_id_t page_count_;
};

}  // namespace

/*
** SALVAGE PAGE SCAN
**
** A normal open requires a valid committed root and complete ownership audit.
** Salvage instead treats each checksummed leaf as an independent source of
** candidate rows.  When allocator metadata remains readable, known free and
** allocator pages are excluded so obsolete page contents are not revived.
** If two leaf pages contain one key, the page with the newer physical LSN
** wins; equal LSNs keep the lower page ID encountered first.
**
** The destination uses the ordinary transaction/WAL path.  Consequently the
** recovered file is a normal TinyDB database even though the source's global
** transaction history could not be proven.
*/
auto Run(const std::filesystem::path &source, const std::filesystem::path &destination,
         Options destination_options) -> Result<Report> {
  if (destination_options.page_cache_bytes < PAGE_SIZE ||
      destination_options.max_write_transaction_bytes < PAGE_SIZE) {
    return std::unexpected(Status::InvalidArgument("salvage resource budgets must hold at least one page"));
  }
  auto filesystem_error = std::error_code{};
  if (std::filesystem::exists(destination, filesystem_error)) {
    return std::unexpected(Status::InvalidArgument("salvage destination already exists"));
  }
  if (filesystem_error) {
    return std::unexpected(Status::IoError("inspect salvage destination: " + filesystem_error.message()));
  }

  auto source_fd = UniqueFd(io::Open(source, O_RDONLY | O_CLOEXEC));
  if (!source_fd.Valid()) {
    return std::unexpected(io::ErrnoStatus("open salvage source"));
  }
  // Salvage has no transaction snapshot. A shared non-blocking file lock
  // therefore rejects a live TinyDB owner rather than mixing checkpoint pages
  // from different instants during the raw scan.
  if (io::Flock(source_fd.Get(), LOCK_SH | LOCK_NB) < 0) {
    if (errno == EWOULDBLOCK) {
      return std::unexpected(Status::Busy("salvage source is open by another process"));
    }
    return std::unexpected(io::ErrnoStatus("lock salvage source"));
  }
  struct stat file_stat {};
  if (io::Fstat(source_fd.Get(), &file_stat) < 0) {
    return std::unexpected(io::ErrnoStatus("fstat salvage source"));
  }
  if (file_stat.st_size < 0) {
    return std::unexpected(Status::Corruption("salvage source has a negative file size"));
  }
  const auto physical_pages = static_cast<std::uint64_t>(file_stat.st_size) / PAGE_SIZE;
  if (physical_pages <= FIRST_DATA_PAGE_ID) {
    return std::unexpected(Status::Corruption("salvage source contains no complete data page"));
  }

  auto selected = Result<storage::SelectedSuperblock>{std::unexpected(Status::Corruption("no valid superblock"))};
  const auto superblock_a = ReadPhysicalPage(source_fd.Get(), SUPERBLOCK_A_PAGE_ID);
  const auto superblock_b = ReadPhysicalPage(source_fd.Get(), SUPERBLOCK_B_PAGE_ID);
  if (superblock_a && superblock_b) {
    selected = storage::SelectSuperblock(std::as_bytes(std::span{**superblock_a}),
                                         std::as_bytes(std::span{**superblock_b}));
  }

  const auto page_count = static_cast<page_id_t>(physical_pages);
  auto scan_frontier = page_count;
  auto state = txn::DatabaseState{};
  auto state_available = false;
  if (selected && selected->value.high_water_page_id <= page_count) {
    const auto &superblock = selected->value;
    state = txn::DatabaseState{
        .root_page_id = superblock.root_page_id,
        .allocator_root_page_id = superblock.allocator_root_page_id,
        .high_water_page_id = superblock.high_water_page_id,
        .transaction_id = superblock.transaction_id,
        .visible_lsn = superblock.checkpoint_lsn,
        .checkpoint_lsn = superblock.checkpoint_lsn,
    };
    scan_frontier = state.high_water_page_id;
    state_available = true;
  }
  auto pages = RawPages(std::move(source_fd), page_count);

  auto report = Report{};
  report.superblock_available = state_available;
  auto excluded = std::unordered_set<page_id_t>{};
  if (state_available) {
    auto allocator = txn::TransactionPages::Begin(&pages, state, destination_options.max_write_transaction_bytes);
    if (allocator) {
      report.allocator_filter_available = true;
      excluded.insert(allocator->AllocatorPageIds().begin(), allocator->AllocatorPageIds().end());
      for (const auto &extent : allocator->FreeExtents()) {
        for (page_id_t page_id = extent.first_page_id; page_id < extent.first_page_id + extent.page_count; ++page_id) {
          excluded.insert(page_id);
        }
      }
    }
  }

  auto rows = RecoveredRows{};
  for (page_id_t page_id = FIRST_DATA_PAGE_ID; page_id < scan_frontier; ++page_id) {
    ++report.pages_scanned;
    if (excluded.contains(page_id)) {
      continue;
    }
    auto page = pages.Read(page_id);
    if (!page) {
      ++report.damaged_pages;
      continue;
    }
    if (RawNodeType(page->Data()) != static_cast<std::uint16_t>(NodeType::Leaf)) {
      continue;
    }
    const auto leaf = LeafPageView::Open(page->Data(), page->Id());
    if (!leaf) {
      ++report.damaged_pages;
      continue;
    }
    ++report.valid_leaf_pages;
    const auto header = storage::DecodeDataPageHeader(
        std::as_bytes(std::span<const char, PAGE_SIZE>{page->Data(), PAGE_SIZE}), page_id);
    if (!header) {
      ++report.damaged_pages;
      continue;
    }
    const auto page_lsn = header->page_lsn;
    for (std::size_t index = 0; index < leaf->Count(); ++index) {
      auto value = CopyValue(&pages, leaf->ValueAt(index));
      if (!value) {
        ++report.damaged_values;
        continue;
      }
      auto key = std::string{leaf->KeyAt(index)};
      const auto row = rows.find(key);
      if (row == rows.end()) {
        rows.emplace(std::move(key), Candidate{page_lsn, std::move(*value)});
      } else {
        ++report.duplicate_rows;
        if (page_lsn > row->second.page_lsn) {
          row->second = Candidate{page_lsn, std::move(*value)};
        }
      }
    }
  }

  auto destination_database = Database::Open(destination, destination_options);
  if (!destination_database) {
    return std::unexpected(destination_database.error());
  }
  for (const auto &[key, candidate] : rows) {
    if (auto status = destination_database->Put(key, candidate.value); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    ++report.rows_recovered;
  }
  if (auto status = destination_database->Close(); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return report;
}

}  // namespace tinydb::salvage
