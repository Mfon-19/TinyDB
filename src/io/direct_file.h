#pragma once

#include <tinydb/status.h>

#include "io/direct_io_fork_gate.h"
#include "io/unique_fd.h"
#include "storage/page.h"

#include <sys/stat.h>
#include <sys/uio.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <utility>

namespace tinydb::io {

/*
** O_DIRECT requires aligned transfer memory. DirectPageBuffer keeps that
** constraint at the transport boundary instead of imposing page alignment on
** every buffered cache and codec allocation.
*/
struct alignas(PAGE_SIZE) DirectPageBuffer final {
  std::array<std::byte, PAGE_SIZE> bytes;
};

static_assert(sizeof(DirectPageBuffer) == PAGE_SIZE);
static_assert(alignof(DirectPageBuffer) == PAGE_SIZE);

inline constexpr std::size_t MAX_DIRECT_WRITE_BATCH_PAGES = 32;
/* A fully scattered prepared read must still fit the reactor's SQE budget. */
inline constexpr std::size_t MAX_DIRECT_READ_BATCH_PAGES = 64;
inline constexpr std::size_t MAX_DIRECT_READ_PAGES_PER_SQE = 32;

/*
** ASYNCHRONOUS DIRECT TRANSFER TOKENS
**
** The native I/O engine borrows the DirectFile descriptor and diagnostic path
** through these tokens. It must complete every token before DirectFile moves
** or is destroyed. Each token also owns one fork-gate admission, released only
** after the kernel stops using the transfer memory. The caller keeps the file
** and all submitted buffers alive until completion, and consumes each token
** exactly once. Short reads report corruption; short writes report I/O error.
*/
class DirectReadRequest final {
 public:
  DirectReadRequest(const DirectReadRequest &) = delete;
  auto operator=(const DirectReadRequest &) -> DirectReadRequest & = delete;
  DirectReadRequest(DirectReadRequest &&other) noexcept;
  auto operator=(DirectReadRequest &&other) noexcept -> DirectReadRequest &;

  auto Descriptor() const noexcept -> int { return descriptor_; }
  auto ByteCount() const noexcept -> std::size_t { return page_count_ * PAGE_SIZE; }
  auto Complete(std::span<const int> completion_results, std::span<const std::size_t> expected_bytes) && -> Status;

 private:
  DirectReadRequest(int descriptor, std::size_t page_count, const std::filesystem::path *path,
                    DirectIoOperation admission)
      : descriptor_(descriptor), page_count_(page_count), path_(path), admission_(std::move(admission)) {}

  int descriptor_{-1};
  std::size_t page_count_{0};
  const std::filesystem::path *path_{nullptr};
  DirectIoOperation admission_;

  friend class DirectFile;
};

class DirectWriteRequest final {
 public:
  DirectWriteRequest(const DirectWriteRequest &) = delete;
  auto operator=(const DirectWriteRequest &) -> DirectWriteRequest & = delete;
  DirectWriteRequest(DirectWriteRequest &&other) noexcept;
  auto operator=(DirectWriteRequest &&other) noexcept -> DirectWriteRequest &;

  auto Descriptor() const noexcept -> int { return descriptor_; }
  auto Complete(int completion_result, std::size_t expected_bytes) && -> Status;

 private:
  DirectWriteRequest(int descriptor, std::size_t page_count, const std::filesystem::path *path,
                     DirectIoOperation admission)
      : descriptor_(descriptor), page_count_(page_count), path_(path), admission_(std::move(admission)) {}

  int descriptor_{-1};
  std::size_t page_count_{0};
  const std::filesystem::path *path_{nullptr};
  DirectIoOperation admission_;

  friend class DirectFile;
};

/*
** DirectFile owns the O_DIRECT descriptor used for database pages and never
** changes to buffered I/O. Open requires the kernel to report memory and
** offset alignment compatible with PAGE_SIZE. Buffered database pages, the
** WAL, and directory synchronization use other descriptor types.
*/
class DirectFile final {
 public:
  static auto OpenExisting(const std::filesystem::path &path, int access_flags) -> Result<DirectFile>;
  static auto CreateExclusive(const std::filesystem::path &path, int access_flags,
                              mode_t create_mode) -> Result<DirectFile>;

  static auto OpenExistingIfPresent(const std::filesystem::path &path,
                                    int access_flags) -> Result<std::optional<DirectFile>>;
  static auto CreateExclusiveIfAbsent(const std::filesystem::path &path, int access_flags,
                                      mode_t create_mode) -> Result<std::optional<DirectFile>>;

  DirectFile(const DirectFile &) = delete;
  auto operator=(const DirectFile &) -> DirectFile & = delete;
  DirectFile(DirectFile &&) noexcept = default;
  auto operator=(DirectFile &&) noexcept -> DirectFile & = default;
  ~DirectFile() = default;

  auto ReadPage(page_id_t page_id, std::span<std::byte, PAGE_SIZE> page) const -> Status;
  auto WritePage(page_id_t page_id, std::span<const std::byte, PAGE_SIZE> page) const -> Status;
  // Write one physically consecutive run. Aligned page sources go directly to
  // pwritev; a mixed or unaligned run uses one bounded staging array.
  auto WritePages(page_id_t first_page_id, std::span<const std::byte *const> pages) const -> Status;
  // Preparation validates every buffer and acquires one fork admission. The
  // caller retains the buffers until it consumes the returned token.
  auto BeginReadPages(std::span<const page_id_t> page_ids,
                      std::span<std::byte> contiguous_pages) const -> Result<DirectReadRequest>;
  auto BeginWritePages(page_id_t first_page_id,
                       std::span<const struct iovec> vectors) const -> Result<DirectWriteRequest>;
  auto EnsurePageCount(page_id_t page_count) const -> Status;
  auto Stat(struct stat *result) const -> Status;
  auto Sync() const -> Status;
  // Drop page-cache residency left by an earlier buffered open; failure of
  // this advisory operation does not change O_DIRECT correctness.
  void DiscardBufferedResidency() const noexcept;

 private:
  DirectFile(UniqueFd fd, std::filesystem::path path, std::size_t memory_alignment, std::size_t offset_alignment)
      : fd_(std::move(fd)),
        path_(std::move(path)),
        memory_alignment_(memory_alignment),
        offset_alignment_(offset_alignment) {}

  static auto Open(const std::filesystem::path &path, int access_flags, bool create, mode_t create_mode,
                   int absent_error) -> Result<std::optional<DirectFile>>;
  auto Transfer(page_id_t page_id, std::byte *read_page, const std::byte *write_page) const -> Status;

  UniqueFd fd_;
  std::filesystem::path path_;
  std::size_t memory_alignment_{};
  std::size_t offset_alignment_{};
};

}  // namespace tinydb::io
