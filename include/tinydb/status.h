#pragma once

#include <expected>
#include <string>
#include <utility>

namespace tinydb {

/*
** ERROR MODEL
**
** Status and Result<T> describe failures that an embedding application is
** expected to handle, including invalid input, resource exhaustion, I/O
** failure, unsupported formats, and persistent corruption. In particular,
** malformed bytes read from disk remain inside this error model instead of
** becoming failed implementation assertions.
**
** These types do not replace the usual C++ rules for constructing owning
** values. An allocation made while producing a result may still throw
** std::bad_alloc.
**
** TINYDB_CHECK is outside this model. It marks an internal programming error,
** such as releasing one page lease twice or performing an impossible state
** transition. Returning Status after such a failure would tell the caller
** that recovery is possible even though an implementation invariant no
** longer holds.
**
** Commit failures need two distinct statuses. IndeterminateCommit reports
** that the durability boundary may have been crossed. NeedsRecovery reports
** that the handle can no longer answer safely and must be reopened before the
** application inspects the transaction outcome.
*/

enum class StatusCode {
  Ok,
  Busy,
  IoError,
  Corruption,
  UnsupportedFormat,
  InvalidArgument,
  ResourceExhausted,
  IndeterminateCommit,
  NeedsRecovery,
  Closed,
};

/*
** The result of an operation that produces no value. Marking the class itself
** nodiscard also marks every function returning Status by value. This makes
** an accidentally ignored environmental or corruption error a compiler
** diagnostic instead of a silent continuation.
*/
class [[nodiscard]] Status {
 public:
  Status() = default;

  auto Ok() const -> bool { return code_ == StatusCode::Ok; }
  auto Code() const -> StatusCode { return code_; }
  auto Message() const -> const std::string & { return message_; }

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

template <typename T>
using Result = std::expected<T, Status>;

}  // namespace tinydb
