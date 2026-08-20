/*
 * A Status encapsulates the result of an operation.
 * Result is an alias for the C++23 std::expected<T, E>
 *
 * For functions not returning a value, we just return a Status,
 * if a function produces a value on success, we wrap the return
 * type with Result<T> and then the caller can check for Status.
 */

#include <algorithm>
#include <expected>
#include <string_view>

namespace tinydb {

enum Code { OK = 0, IoError = 1 };

class Status {
public:
  Status() = default; // Ok

  auto Ok() -> const bool { return code_ == Code::OK; } // for callers
  auto Message() -> const std::string_view { return message_; }

  static auto IoError(std::string_view message) -> Status {
    return {Code::IoError, message};
  }

private:
  Status(Code code, std::string_view message)
      : code_(code), message_(message) {}

  enum Code code_ = Code::OK;
  std::string_view message_;
};

template <typename T, typename E = Status> 
using Result = std::expected<T, E>;

// This allows us to avoid writing std::unexpected(...) every time
template <typename E = Status> 
inline auto Err(E &&error) {
  return std::unexpected(std::forward<E>(error));
}
} // namespace tinydb