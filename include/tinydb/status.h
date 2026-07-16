#pragma once

#include <expected>
#include <string>
#include <utility>

namespace tinydb {

/*
** ERROR MODEL
**
** Status and Result<T> carry every failure an embedding application might
** reasonably encounter: invalid input, resource exhaustion, I/O failure,
** unsupported formats, and detected persistent corruption. TinyDB does not
** throw exceptions across its API and does not terminate the host merely
** because bytes read from disk are malformed.
**
** TINYDB_CHECK is deliberately outside this model. It is reserved for an
** internal programming error, such as releasing one page lease twice or
** performing an impossible state transition. Turning such a bug into a
** Status would make the database appear recoverable when its implementation
** invariant has already failed.
**
** Commit failures need two distinct statuses. IndeterminateCommit reports
** that the durability boundary may have been crossed. NeedsRecovery reports
** that the handle can no longer answer safely and must be reopened before the
** application inspects the transaction outcome.
*/

enum class StatusCode {
  Ok,
  Busy,                 // an exclusive resource is currently owned elsewhere
  IoError,              // the environment failed: read, write, sync, open
  Corruption,           // on-disk state cannot be trusted
  UnsupportedFormat,    // persistent bytes use a format this binary cannot read
  InvalidArgument,      // the caller asked for something impossible
  ResourceExhausted,    // a fixed resource ran out (e.g. evictable frames)
  IndeterminateCommit,  // a commit may or may not have crossed durability
  NeedsRecovery,        // reopen is required before the outcome can be known
  Closed,               // the handle no longer accepts work
};

/*
** The result of an operation that produces no value. Marking the class itself
** nodiscard also marks every function returning Status by value. This makes
** an accidentally ignored environmental or corruption error a compiler
** diagnostic instead of a silent continuation.
*/
class [[nodiscard]] Status {
 public:
  Status() = default;  // Ok

  auto Ok() const -> bool { return code_ == StatusCode::Ok; }
  auto Code() const -> StatusCode { return code_; }
  auto Message() const -> const std::string & { return message_; }

  // "IO error: pwrite: No space left on device" — for logs and CLI output.
  auto ToString() const -> std::string {
    if (Ok()) {
      return "OK";
    }
    return std::string(CodeName(code_)) + ": " + message_;
  }

  static auto Busy(std::string message) -> Status { return {StatusCode::Busy, std::move(message)}; }
  static auto IoError(std::string message) -> Status { return {StatusCode::IoError, std::move(message)}; }
  static auto Corruption(std::string message) -> Status { return {StatusCode::Corruption, std::move(message)}; }
  static auto UnsupportedFormat(std::string message) -> Status {
    return {StatusCode::UnsupportedFormat, std::move(message)};
  }
  static auto InvalidArgument(std::string message) -> Status {
    return {StatusCode::InvalidArgument, std::move(message)};
  }
  static auto ResourceExhausted(std::string message) -> Status {
    return {StatusCode::ResourceExhausted, std::move(message)};
  }
  static auto IndeterminateCommit(std::string message) -> Status {
    return {StatusCode::IndeterminateCommit, std::move(message)};
  }
  static auto NeedsRecovery(std::string message) -> Status { return {StatusCode::NeedsRecovery, std::move(message)}; }
  static auto Closed(std::string message) -> Status { return {StatusCode::Closed, std::move(message)}; }

 private:
  Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

  static auto CodeName(StatusCode code) -> const char * {
    switch (code) {
      case StatusCode::Ok:
        return "OK";
      case StatusCode::Busy:
        return "busy";
      case StatusCode::IoError:
        return "IO error";
      case StatusCode::Corruption:
        return "corruption";
      case StatusCode::UnsupportedFormat:
        return "unsupported format";
      case StatusCode::InvalidArgument:
        return "invalid argument";
      case StatusCode::ResourceExhausted:
        return "resource exhausted";
      case StatusCode::IndeterminateCommit:
        return "indeterminate commit";
      case StatusCode::NeedsRecovery:
        return "needs recovery";
      case StatusCode::Closed:
        return "closed";
    }
    return "unknown";
  }

  StatusCode code_{StatusCode::Ok};
  std::string message_;
};

/* A value-producing operation returns either the value or its Status. */
template <typename T>
using Result = std::expected<T, Status>;

}  // namespace tinydb
