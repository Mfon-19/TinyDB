#pragma once

#include <tinydb/bytes.h>
#include <tinydb/status.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace tinydb {

class ReadTransaction;

/*
** ORDERED RANGE DESCRIPTION
**
** Bounds are owned because a Cursor may outlive the strings used to create
** it. Between is half-open: [lower, upper). From and Until omit one bound.
** Prefix computes the smallest exclusive bytewise successor when one exists;
** an all-0xff prefix has no upper bound.
*/
class KeyRange final {
 public:
  KeyRange() = default;

  static auto All() -> KeyRange;
  static auto From(BytesView lower) -> KeyRange;
  static auto Until(BytesView upper) -> KeyRange;
  static auto Between(BytesView lower, BytesView upper) -> KeyRange;
  static auto Prefix(BytesView prefix) -> KeyRange;

  auto Lower() const -> std::optional<BytesView>;
  auto Upper() const -> std::optional<BytesView>;

 private:
  KeyRange(std::optional<Bytes> lower, std::optional<Bytes> upper)
      : lower_(std::move(lower)), upper_(std::move(upper)) {}

  std::optional<Bytes> lower_;
  std::optional<Bytes> upper_;
};

/*
** A Cursor is one streaming view of a read snapshot. Key borrows the currently
** pinned leaf and remains valid until First, Seek, Next, or destruction.
** CopyValue returns owned bytes. End of range is Valid()==false after a
** successful movement operation; I/O and corruption remain Status failures.
**
** The cursor retains both snapshot admission and database-core lifetime. It
** may outlive the ReadTransaction that created it, and Database::Close returns
** Busy until the cursor is destroyed.
*/
class Cursor final {
 public:
  Cursor(const Cursor &) = delete;
  auto operator=(const Cursor &) -> Cursor & = delete;
  Cursor(Cursor &&) noexcept;
  auto operator=(Cursor &&) -> Cursor & = delete;
  ~Cursor();

  auto First() -> Status;
  auto Seek(BytesView key) -> Status;
  auto Next() -> Status;
  auto Valid() const -> bool;
  auto Key() const -> BytesView;
  auto ValueSize() const -> std::uint64_t;
  auto CopyValue() const -> Result<Bytes>;

 private:
  struct Impl;
  explicit Cursor(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  friend class ReadTransaction;
};

}  // namespace tinydb
