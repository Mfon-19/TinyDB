#include "io/direct_file.h"

#include "io/direct_io_fork_gate.h"
#include "io/testable_posix.h"
#include "util/check.h"

#include <fcntl.h>
#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace tinydb::io {
namespace {

struct DirectIoGeometry final {
  std::size_t memory_alignment;
  std::size_t offset_alignment;
};

auto PathError(std::string_view operation, const std::filesystem::path &path, int error) -> Status {
  return Status::IoError(std::string(operation) + " " + path.string() + ": " + std::generic_category().message(error));
}

auto PageOffset(page_id_t page_id) -> Result<std::uint64_t> {
  constexpr auto off_t_max = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
  constexpr auto last_page_start = off_t_max - (PAGE_SIZE - 1U);
  if (page_id > last_page_start / PAGE_SIZE) {
    return std::unexpected(Status::InvalidArgument("database page offset exceeds off_t"));
  }
  return page_id * PAGE_SIZE;
}

auto ValidateAccessFlags(int access_flags) -> Status {
  if (access_flags != O_RDONLY && access_flags != O_RDWR) {
    return Status::InvalidArgument("DirectFile access flags must be exactly O_RDONLY or O_RDWR");
  }
  return {};
}

#if defined(__linux__) && defined(O_DIRECT) && defined(STATX_DIOALIGN) && defined(AT_EMPTY_PATH)
auto ValidateGeometry(const struct statx &file_stat, const std::filesystem::path &path) -> Result<DirectIoGeometry> {
  const auto report = " (stx_mask=" + std::to_string(file_stat.stx_mask) +
                      ", memory_alignment=" + std::to_string(file_stat.stx_dio_mem_align) +
                      ", offset_alignment=" + std::to_string(file_stat.stx_dio_offset_align) + ")";
  if ((file_stat.stx_mask & STATX_DIOALIGN) == 0) {
    return std::unexpected(Status::IoError("STATX_DIOALIGN was not reported for " + path.string() + report));
  }

  const auto memory_alignment = static_cast<std::size_t>(file_stat.stx_dio_mem_align);
  const auto offset_alignment = static_cast<std::size_t>(file_stat.stx_dio_offset_align);
  if (memory_alignment == 0 || offset_alignment == 0) {
    return std::unexpected(Status::IoError("direct I/O is unsupported for " + path.string() + report));
  }
  if (PAGE_SIZE % memory_alignment != 0 || PAGE_SIZE % offset_alignment != 0) {
    return std::unexpected(
        Status::IoError("direct-I/O alignment is incompatible with TinyDB pages for " + path.string() + report));
  }
  return DirectIoGeometry{
      .memory_alignment = memory_alignment,
      .offset_alignment = offset_alignment,
  };
}
#endif

enum class TransferFailure {
  None,
  System,
  Eof,
  ZeroProgress,
  UnalignedProgress,
};

struct TransferResult final {
  TransferFailure failure{TransferFailure::None};
  int error{0};
};

}  // namespace

auto DirectFile::OpenExisting(const std::filesystem::path &path, int access_flags) -> Result<DirectFile> {
  auto opened = OpenExistingIfPresent(path, access_flags);
  if (!opened) {
    return std::unexpected(opened.error());
  }
  if (!opened->has_value()) {
    return std::unexpected(Status::IoError("database does not exist: " + path.string()));
  }
  return std::move(opened->value());  // NOLINT(bugprone-unchecked-optional-access)
}

auto DirectFile::CreateExclusive(const std::filesystem::path &path, int access_flags,
                                 mode_t create_mode) -> Result<DirectFile> {
  auto created = CreateExclusiveIfAbsent(path, access_flags, create_mode);
  if (!created) {
    return std::unexpected(created.error());
  }
  if (!created->has_value()) {
    return std::unexpected(Status::IoError("database already exists: " + path.string()));
  }
  return std::move(created->value());  // NOLINT(bugprone-unchecked-optional-access)
}

auto DirectFile::OpenExistingIfPresent(const std::filesystem::path &path,
                                       int access_flags) -> Result<std::optional<DirectFile>> {
  return Open(path, access_flags, false, 0, ENOENT);
}

auto DirectFile::CreateExclusiveIfAbsent(const std::filesystem::path &path, int access_flags,
                                         mode_t create_mode) -> Result<std::optional<DirectFile>> {
  return Open(path, access_flags, true, create_mode, EEXIST);
}

auto DirectFile::Open(const std::filesystem::path &path, int access_flags, bool create, mode_t create_mode,
                      int absent_error) -> Result<std::optional<DirectFile>> {
  if (auto status = ValidateAccessFlags(access_flags); !status.Ok()) {
    return std::unexpected(std::move(status));
  }

#if defined(__linux__) && defined(O_DIRECT) && defined(STATX_DIOALIGN) && defined(AT_EMPTY_PATH)
  if (auto status = EnsureDirectIoForkGate(); !status.Ok()) {
    return std::unexpected(std::move(status));
  }

  // Direct mode is fail-closed. The descriptor is usable only after the
  // kernel reports alignment that supports one complete TinyDB page.
  const auto flags = access_flags | O_DIRECT | O_CLOEXEC | (create ? O_CREAT | O_EXCL : 0);
  const auto descriptor = io::Open(path, flags, create_mode);
  if (descriptor < 0) {
    const auto error = errno;
    if (error == absent_error) {
      return std::optional<DirectFile>{};
    }
    return std::unexpected(PathError("open", path, error));
  }
  auto fd = UniqueFd(descriptor);

  struct stat descriptor_stat {};
  if (Fstat(fd.Get(), &descriptor_stat) < 0) {
    return std::unexpected(PathError("fstat", path, errno));
  }
  if (!S_ISREG(descriptor_stat.st_mode)) {
    return std::unexpected(Status::InvalidArgument("database is not a regular file: " + path.string()));
  }

  struct statx direct_stat {};
  if (Statx(fd.Get(), "", AT_EMPTY_PATH, STATX_DIOALIGN, &direct_stat) < 0) {
    const auto failure = PathError("statx", path, errno);
    return std::unexpected(Status::IoError(
        failure.Message() + " (stx_mask=unavailable, memory_alignment=unavailable, offset_alignment=unavailable)"));
  }
  auto geometry = ValidateGeometry(direct_stat, path);
  if (!geometry) {
    return std::unexpected(geometry.error());
  }
  return std::optional{DirectFile(std::move(fd), path, geometry->memory_alignment, geometry->offset_alignment)};
#else
  static_cast<void>(path);
  static_cast<void>(create);
  static_cast<void>(create_mode);
  static_cast<void>(absent_error);
  return std::unexpected(Status::IoError("direct I/O is not supported by this build"));
#endif
}

DirectReadRequest::DirectReadRequest(DirectReadRequest &&other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)),
      page_count_(std::exchange(other.page_count_, 0)),
      path_(std::exchange(other.path_, nullptr)),
      admission_(std::move(other.admission_)) {}

auto DirectReadRequest::operator=(DirectReadRequest &&other) noexcept -> DirectReadRequest & {
  if (this != &other) {
    const auto empty = descriptor_ < 0 && page_count_ == 0 && path_ == nullptr;
    TINYDB_CHECK(empty, "overwriting an incomplete asynchronous direct read");
    descriptor_ = std::exchange(other.descriptor_, -1);
    page_count_ = std::exchange(other.page_count_, 0);
    path_ = std::exchange(other.path_, nullptr);
    admission_ = std::move(other.admission_);
  }
  return *this;
}

auto DirectReadRequest::Complete(std::span<const int> completion_results,
                                 std::span<const std::size_t> expected_bytes) && -> Status {
  const auto valid_request = descriptor_ >= 0 && path_ != nullptr && page_count_ != 0;
  TINYDB_CHECK(valid_request, "completing an invalid asynchronous direct read");
  const auto valid_layout = !completion_results.empty() && completion_results.size() == expected_bytes.size();
  TINYDB_CHECK(valid_layout, "asynchronous direct read has an invalid completion layout");

  const auto *const path = path_;
  const auto byte_count = ByteCount();
  descriptor_ = -1;
  page_count_ = 0;
  path_ = nullptr;
  admission_ = DirectIoOperation{};

  // The reactor can split one logical run across many SQEs. Each CQE must
  // cover its complete aligned segment.
  auto described_bytes = std::size_t{0};
  auto first_error = 0;
  auto short_read = false;
  for (auto index = std::size_t{0}; index < completion_results.size(); ++index) {
    const auto expected = expected_bytes[index];
    const auto valid_expected_bytes =
        expected != 0 && expected % PAGE_SIZE == 0 && expected <= byte_count - described_bytes;
    TINYDB_CHECK(valid_expected_bytes, "asynchronous direct read has an invalid expected byte count");
    described_bytes += expected;
    const auto result = completion_results[index];
    if (result == static_cast<int>(expected)) {
      continue;
    }
    if (result < 0 && first_error == 0) {
      first_error = -result;
    } else {
      TINYDB_CHECK(result < static_cast<int>(expected), "asynchronous direct read exceeded its request");
      short_read = true;
    }
  }
  TINYDB_CHECK(described_bytes == byte_count, "asynchronous direct read layout omits page bytes");
  if (first_error != 0) {
    return PathError("io_uring read", *path, first_error);
  }
  if (short_read) {
    return Status::Corruption("short asynchronous read on a persistent page: " + path->string());
  }
  return {};
}

DirectWriteRequest::DirectWriteRequest(DirectWriteRequest &&other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)),
      page_count_(std::exchange(other.page_count_, 0)),
      path_(std::exchange(other.path_, nullptr)),
      admission_(std::move(other.admission_)) {}

auto DirectWriteRequest::operator=(DirectWriteRequest &&other) noexcept -> DirectWriteRequest & {
  if (this != &other) {
    const auto empty = descriptor_ < 0 && page_count_ == 0 && path_ == nullptr;
    TINYDB_CHECK(empty, "overwriting an incomplete asynchronous direct write");
    descriptor_ = std::exchange(other.descriptor_, -1);
    page_count_ = std::exchange(other.page_count_, 0);
    path_ = std::exchange(other.path_, nullptr);
    admission_ = std::move(other.admission_);
  }
  return *this;
}

auto DirectWriteRequest::Complete(int completion_result, std::size_t expected_bytes) && -> Status {
  const auto valid_request =
      descriptor_ >= 0 && path_ != nullptr && page_count_ != 0 && expected_bytes == page_count_ * PAGE_SIZE;
  TINYDB_CHECK(valid_request, "completing an invalid asynchronous direct write");
  const auto *const path = path_;
  descriptor_ = -1;
  page_count_ = 0;
  path_ = nullptr;
  admission_ = DirectIoOperation{};
  if (completion_result == static_cast<int>(expected_bytes)) {
    return {};
  }
  if (completion_result < 0) {
    return PathError("io_uring writev", *path, -completion_result);
  }
  // io_uring does not resume a partial checkpoint write. A short completion
  // leaves the checkpoint transfer incomplete.
  TINYDB_CHECK(completion_result < static_cast<int>(expected_bytes), "asynchronous direct write exceeded its request");
  return Status::IoError("short asynchronous checkpoint write: " + path->string());
}

auto DirectFile::ReadPage(page_id_t page_id, std::span<std::byte, PAGE_SIZE> page) const -> Status {
  const auto valid_file = fd_.Valid() && memory_alignment_ != 0;
  TINYDB_CHECK(valid_file, "reading through a moved-from DirectFile");
  const auto aligned = reinterpret_cast<std::uintptr_t>(page.data()) % memory_alignment_ == 0;
  if (aligned) {
    return Transfer(page_id, page.data(), nullptr);
  }
  // Direct arena pages avoid this copy. An ordinary caller page uses one
  // aligned staging page for the kernel transfer.
  auto staging = DirectPageBuffer{};
  if (auto status = Transfer(page_id, staging.bytes.data(), nullptr); !status.Ok()) {
    return status;
  }
  std::ranges::copy(staging.bytes, page.begin());
  return {};
}

auto DirectFile::WritePage(page_id_t page_id, std::span<const std::byte, PAGE_SIZE> page) const -> Status {
  const auto valid_file = fd_.Valid() && memory_alignment_ != 0;
  TINYDB_CHECK(valid_file, "writing through a moved-from DirectFile");
  const auto aligned = reinterpret_cast<std::uintptr_t>(page.data()) % memory_alignment_ == 0;
  if (aligned) {
    return Transfer(page_id, nullptr, page.data());
  }
  // Copy before the transfer because the kernel cannot read an unaligned
  // caller page through O_DIRECT.
  auto staging = DirectPageBuffer{};
  std::ranges::copy(page, staging.bytes.begin());
  return Transfer(page_id, nullptr, staging.bytes.data());
}

auto DirectFile::WritePages(page_id_t first_page_id, std::span<const std::byte *const> pages) const -> Status {
  TINYDB_CHECK(fd_.Valid(), "writing through a moved-from DirectFile");
  TINYDB_CHECK(memory_alignment_ != 0, "direct file has no memory alignment");
  if (pages.empty() || pages.size() > MAX_DIRECT_WRITE_BATCH_PAGES ||
      pages.size() - 1U > std::numeric_limits<page_id_t>::max() - first_page_id) {
    return Status::InvalidArgument("direct write batch has an invalid page range");
  }
  const auto offset = PageOffset(first_page_id);
  const auto last_offset = PageOffset(first_page_id + pages.size() - 1U);
  if (!offset || !last_offset) {
    return !offset ? offset.error() : last_offset.error();
  }
  constexpr auto maximum_transfer = static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
  if (pages.size() > maximum_transfer / PAGE_SIZE) {
    return Status::InvalidArgument("direct write batch exceeds ssize_t");
  }
  if (std::ranges::any_of(pages, [](const auto *page) { return page == nullptr; })) {
    return Status::InvalidArgument("direct write batch contains a null page");
  }

  // A mixed batch stages the full run. This gives every retry one uniform
  // aligned vector layout.
  auto staging = std::unique_ptr<DirectPageBuffer[]>{};
  const auto sources_are_aligned = std::ranges::all_of(
      pages, [&](const auto *page) { return reinterpret_cast<std::uintptr_t>(page) % memory_alignment_ == 0; });
  if (!sources_are_aligned) {
    try {
      staging = std::make_unique_for_overwrite<DirectPageBuffer[]>(pages.size());
    } catch (const std::bad_alloc &) {
      return Status::ResourceExhausted("allocating aligned direct-write staging pages");
    }
    for (auto index = std::size_t{0}; index < pages.size(); ++index) {
      std::ranges::copy_n(pages[index], PAGE_SIZE, staging[index].bytes.begin());
    }
  }

  auto base_vectors = std::array<struct iovec, MAX_DIRECT_WRITE_BATCH_PAGES>{};
  for (auto index = std::size_t{0}; index < pages.size(); ++index) {
    const auto *const source = staging == nullptr ? pages[index] : staging[index].bytes.data();
    base_vectors[index] = iovec{.iov_base = const_cast<std::byte *>(source), .iov_len = PAGE_SIZE};
  }
  const auto aligned_transfer = *offset % offset_alignment_ == 0 && PAGE_SIZE % offset_alignment_ == 0;
  TINYDB_CHECK(aligned_transfer, "misaligned direct write batch");

  auto admission = DirectIoOperation::Begin();
  if (!admission) {
    return admission.error();
  }

  auto failure = TransferResult{};
  const auto transfer_bytes = pages.size() * PAGE_SIZE;
  auto total = std::size_t{0};
  {
    auto operation = std::move(*admission);
    // POSIX permits short writes. A retry is valid only while the next vector,
    // file offset, and remaining length still satisfy O_DIRECT.
    while (total < transfer_bytes) {
      const auto first_index = total / PAGE_SIZE;
      const auto within_page = total % PAGE_SIZE;
      auto vectors = std::array<struct iovec, MAX_DIRECT_WRITE_BATCH_PAGES>{};
      auto vector_count = std::size_t{0};
      for (auto index = first_index; index < pages.size(); ++index) {
        auto *base = static_cast<std::byte *>(base_vectors[index].iov_base);
        auto length = base_vectors[index].iov_len;
        if (index == first_index) {
          base += within_page;
          length -= within_page;
        }
        vectors[vector_count++] = iovec{.iov_base = base, .iov_len = length};
      }

      const auto next_address = reinterpret_cast<std::uintptr_t>(vectors.front().iov_base);
      const auto next_offset = *offset + total;
      const auto remaining = transfer_bytes - total;
      if (next_address % memory_alignment_ != 0 || next_offset % offset_alignment_ != 0 ||
          vectors.front().iov_len % offset_alignment_ != 0 || remaining % offset_alignment_ != 0) {
        failure.failure = TransferFailure::UnalignedProgress;
        break;
      }

      const auto transferred =
          Pwritev(fd_.Get(), std::span<const struct iovec>{vectors.data(), vector_count}, next_offset);
      if (transferred < 0) {
        if (errno == EINTR) {
          continue;
        }
        failure = TransferResult{.failure = TransferFailure::System, .error = errno};
        break;
      }
      if (transferred == 0) {
        failure.failure = TransferFailure::ZeroProgress;
        break;
      }
      const auto progress = static_cast<std::size_t>(transferred);
      TINYDB_CHECK(progress <= remaining, "direct pwritev exceeded its requested transfer");
      total += progress;
    }
  }

  switch (failure.failure) {
    case TransferFailure::None:
      return {};
    case TransferFailure::System:
      return PathError("pwritev", path_, failure.error);
    case TransferFailure::ZeroProgress:
      return Status::IoError("direct pwritev made no progress: " + path_.string());
    case TransferFailure::UnalignedProgress:
      return Status::IoError("direct pwritev returned terminal unaligned progress: " + path_.string());
    case TransferFailure::Eof:
      TINYDB_CHECK(false, "vectored write reported end of file");
  }
  TINYDB_CHECK(false, "unknown direct write batch result");
}

auto DirectFile::BeginReadPages(std::span<const page_id_t> page_ids,
                                std::span<std::byte> contiguous_pages) const -> Result<DirectReadRequest> {
  TINYDB_CHECK(fd_.Valid(), "transferring through a moved-from DirectFile");
  if (page_ids.empty() || page_ids.size() > MAX_DIRECT_READ_BATCH_PAGES ||
      page_ids.size() > std::numeric_limits<std::size_t>::max() / PAGE_SIZE ||
      contiguous_pages.size() != page_ids.size() * PAGE_SIZE) {
    return std::unexpected(Status::InvalidArgument("asynchronous direct read has an invalid page layout"));
  }
  if (reinterpret_cast<std::uintptr_t>(contiguous_pages.data()) % memory_alignment_ != 0) {
    return std::unexpected(Status::InvalidArgument("asynchronous direct read buffer is misaligned"));
  }
  for (const auto page_id : page_ids) {
    const auto offset = PageOffset(page_id);
    if (!offset) {
      return std::unexpected(offset.error());
    }
    TINYDB_CHECK(*offset % offset_alignment_ == 0, "misaligned asynchronous database offset");
  }
  TINYDB_CHECK(PAGE_SIZE % offset_alignment_ == 0, "misaligned asynchronous database page length");
  auto admission = DirectIoOperation::Begin();
  if (!admission) {
    return std::unexpected(std::move(admission).error());
  }
  // The request token keeps this admission. Thus, the fork handler waits for
  // queued and active io_uring work.
  return DirectReadRequest(fd_.Get(), page_ids.size(), &path_, std::move(*admission));
}

auto DirectFile::BeginWritePages(page_id_t first_page_id,
                                 std::span<const struct iovec> vectors) const -> Result<DirectWriteRequest> {
  TINYDB_CHECK(fd_.Valid(), "transferring through a moved-from DirectFile");
  if (vectors.empty() || vectors.size() > MAX_DIRECT_WRITE_BATCH_PAGES ||
      vectors.size() - 1U > std::numeric_limits<page_id_t>::max() - first_page_id) {
    return std::unexpected(Status::InvalidArgument("asynchronous direct write has an invalid page range"));
  }
  const auto offset = PageOffset(first_page_id);
  if (!offset) {
    return std::unexpected(offset.error());
  }
  for (const auto &vector : vectors) {
    if (vector.iov_base == nullptr || vector.iov_len != PAGE_SIZE ||
        reinterpret_cast<std::uintptr_t>(vector.iov_base) % memory_alignment_ != 0) {
      return std::unexpected(Status::InvalidArgument("asynchronous direct write contains an invalid page buffer"));
    }
  }
  const auto aligned = *offset % offset_alignment_ == 0 && PAGE_SIZE % offset_alignment_ == 0;
  TINYDB_CHECK(aligned, "misaligned asynchronous database write");
  auto admission = DirectIoOperation::Begin();
  if (!admission) {
    return std::unexpected(std::move(admission).error());
  }
  return DirectWriteRequest(fd_.Get(), vectors.size(), &path_, std::move(*admission));
}

auto DirectFile::EnsurePageCount(page_id_t page_count) const -> Status {
  TINYDB_CHECK(fd_.Valid(), "growing a moved-from DirectFile");
  constexpr auto off_t_max = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
  if (page_count > off_t_max / PAGE_SIZE) {
    return Status::InvalidArgument("database page count exceeds off_t");
  }
  const auto target_bytes = page_count * PAGE_SIZE;

  struct stat file_stat {};
  if (Fstat(fd_.Get(), &file_stat) < 0) {
    return PathError("fstat", path_, errno);
  }
  TINYDB_CHECK(file_stat.st_size >= 0, "regular database file has a negative size");
  if (static_cast<std::uint64_t>(file_stat.st_size) >= target_bytes) {
    return {};
  }
  if (Ftruncate(fd_.Get(), target_bytes) < 0) {
    return PathError("ftruncate", path_, errno);
  }
  return {};
}

auto DirectFile::Stat(struct stat *result) const -> Status {
  TINYDB_CHECK(fd_.Valid(), "inspecting a moved-from DirectFile");
  TINYDB_CHECK(result != nullptr, "DirectFile::Stat requires an output buffer");
  if (Fstat(fd_.Get(), result) < 0) {
    return PathError("fstat", path_, errno);
  }
  return {};
}

auto DirectFile::Sync() const -> Status {
  TINYDB_CHECK(fd_.Valid(), "syncing a moved-from DirectFile");
  if (Fsync(fd_.Get()) < 0) {
    return PathError("fsync", path_, errno);
  }
  return {};
}

void DirectFile::DiscardBufferedResidency() const noexcept {
  TINYDB_CHECK(fd_.Valid(), "discarding residency through a moved-from DirectFile");
#if defined(__linux__)
  static_cast<void>(::posix_fadvise(fd_.Get(), 0, 0, POSIX_FADV_DONTNEED));
#endif
}

auto DirectFile::Transfer(page_id_t page_id, std::byte *read_page, const std::byte *write_page) const -> Status {
  TINYDB_CHECK(fd_.Valid(), "transferring through a moved-from DirectFile");
  TINYDB_CHECK((read_page == nullptr) != (write_page == nullptr), "direct transfer has invalid direction");
  const auto writing = write_page != nullptr;
  const auto offset = PageOffset(page_id);
  if (!offset) {
    return offset.error();
  }

  const auto *const base = writing ? static_cast<const void *>(write_page) : static_cast<const void *>(read_page);
  const auto address = reinterpret_cast<std::uintptr_t>(base);
  TINYDB_CHECK(address % memory_alignment_ == 0, "misaligned direct page buffer");
  TINYDB_CHECK(*offset % offset_alignment_ == 0, "misaligned database page offset");
  TINYDB_CHECK(PAGE_SIZE % offset_alignment_ == 0, "misaligned database page length");

  auto admitted = DirectIoOperation::Begin();
  if (!admitted) {
    return admitted.error();
  }

  auto result = TransferResult{};
  auto total = std::size_t{0};
  {
    // This token keeps the fork gate active for the complete retry loop.
    auto operation = std::move(*admitted);
    // POSIX permits short transfers. Continue only while the next buffer,
    // offset, and length still satisfy direct-I/O alignment.
    while (total < PAGE_SIZE) {
      const auto remaining = PAGE_SIZE - total;
      const auto transferred = writing ? Pwrite(fd_.Get(), write_page + total, remaining, *offset + total)
                                       : Pread(fd_.Get(), read_page + total, remaining, *offset + total);
      if (transferred < 0) {
        if (errno == EINTR) {
          continue;
        }
        result = TransferResult{.failure = TransferFailure::System, .error = errno};
        break;
      }
      if (transferred == 0) {
        result.failure = writing ? TransferFailure::ZeroProgress : TransferFailure::Eof;
        break;
      }

      const auto progress = static_cast<std::size_t>(transferred);
      TINYDB_CHECK(progress <= remaining, "direct syscall exceeded its requested transfer");
      total += progress;
      if (total == PAGE_SIZE) {
        break;
      }

      const auto next_address = address + total;
      const auto next_offset = *offset + total;
      const auto next_length = PAGE_SIZE - total;
      if (next_address % memory_alignment_ != 0 || next_offset % offset_alignment_ != 0 ||
          next_length % offset_alignment_ != 0) {
        result.failure = TransferFailure::UnalignedProgress;
        break;
      }
    }
  }

  switch (result.failure) {
    case TransferFailure::None:
      return {};
    case TransferFailure::System:
      return PathError(writing ? "pwrite" : "pread", path_, result.error);
    case TransferFailure::Eof:
      return Status::Corruption("short read on a persistent page: " + path_.string());
    case TransferFailure::ZeroProgress:
      return Status::IoError("direct pwrite made no progress: " + path_.string());
    case TransferFailure::UnalignedProgress:
      return Status::IoError("direct syscall returned terminal unaligned progress: " + path_.string());
  }
  TINYDB_CHECK(false, "unknown direct transfer result");
}

}  // namespace tinydb::io
