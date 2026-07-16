#pragma once

#include <expected>
#include <string>
#include <utility>

namespace tinydb {

/**
  Error handling in TinyDB is split three ways:

  - Environmental failures (a read, write, sync, or open failed; a fixed
    resource ran out) travel as Status / Result<T> return values. They are
    expected outcomes on a healthy binary and every caller has a decision
    to make about them. Nothing in the library throws.

  - Invariant violations (double free, unpinning an unpinned page) abort
    via TINYDB_CHECK: they only fire on a bug, and a status would launder
    the bug into an "error" no caller can act on.

  - Corruption is both, by when it is found: rejected with a status at open
    time (nothing has been trusted yet), aborted on mid-operation (the
    engine has already acted on state it believed).
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

// The result of an operation that produces no value. nodiscard on the class
// makes every function returning Status by value nodiscard: an ignored
// error is the classic status-code bug.
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
  static auto NeedsRecovery(std::string message) -> Status {
    return {StatusCode::NeedsRecovery, std::move(message)};
  }
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

// The result of an operation that produces a value: the value, or the
// Status explaining why there is none.
template <typename T>
using Result = std::expected<T, Status>;

}  // namespace tinydb
