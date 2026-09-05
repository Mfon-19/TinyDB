#pragma once

/*
 * A Status encapsulates the result of an operation.
 * Result is an alias for the C++23 std::expected<T, Status>.
 *
 * For functions not returning a value, we just return a Status,
 * if a function produces a value on success, we wrap the return
 * type with Result<T> and then the caller can check for Status.
 */

#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace tinydb {

class Status {
public:
  Status() = default; // Ok

  [[nodiscard]] auto Ok() const noexcept -> bool { return code_ == Code::Ok; }
  [[nodiscard]] auto Message() const noexcept -> std::string_view {
    return error_ == 0 ? std::string_view{message_} : std::strerror(error_);
  }

  // Preserve errno without allocating an error message.
  static auto IoError(int error) noexcept -> Status {
    return Status(error);
  }

  static auto IoError(std::string message) -> Status {
    return {Code::IoError, std::move(message)};
  }

  static auto ResourceExhausted(std::string message) -> Status {
    return {Code::ResourceExhausted, std::move(message)};
  }

  static auto InvalidArgument(std::string message) -> Status {
    return {Code::InvalidArgument, std::move(message)};
  }

  static auto Corruption(std::string message) -> Status {
    return {Code::Corruption, std::move(message)};
  }

private:
  enum class Code {
    Ok,
    IoError,
    ResourceExhausted,
    InvalidArgument,
    Corruption
  };

  Status(Code code, std::string message)
      : code_(code), message_(std::move(message)) {}

  explicit Status(int error) noexcept
      : code_(Code::IoError), error_(error) {}

  Code code_ = Code::Ok;
  int error_ = 0;
  std::string message_;
};

template <typename T> using Result = std::expected<T, Status>;

// This allows us to avoid writing std::unexpected(...) every time.
inline auto Err(Status error) -> std::unexpected<Status> {
  return std::unexpected(std::move(error));
}
} // namespace tinydb
