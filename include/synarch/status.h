#pragma once

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace synarch {

enum class StatusCode {
  kOk,
  kInvalidArgument,
  kIoError,
  kCorruption,
  kNotFound,
  kInternal,
};

class Status {
public:
  Status() = default;

  [[nodiscard]] static Status ok() { return Status{}; }

  [[nodiscard]] static Status invalid_argument(std::string message) {
    return Status(StatusCode::kInvalidArgument, std::move(message));
  }

  [[nodiscard]] static Status io_error(std::string message) {
    return Status(StatusCode::kIoError, std::move(message));
  }

  [[nodiscard]] static Status corruption(std::string message) {
    return Status(StatusCode::kCorruption, std::move(message));
  }

  [[nodiscard]] static Status not_found(std::string message) {
    return Status(StatusCode::kNotFound, std::move(message));
  }

  [[nodiscard]] static Status internal(std::string message) {
    return Status(StatusCode::kInternal, std::move(message));
  }

  [[nodiscard]] bool is_ok() const noexcept { return code_ == StatusCode::kOk; }

  [[nodiscard]] StatusCode code() const noexcept { return code_; }

  [[nodiscard]] std::string_view message() const noexcept { return message_; }

private:
  explicit Status(StatusCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  StatusCode code_{StatusCode::kOk};
  std::string message_;
};

template <typename T>
class Result {
  static_assert(!std::is_same_v<std::remove_cv_t<T>, Status>,
                "Result<Status> is ambiguous; use Status directly");

public:
  [[nodiscard]] static Result success(T value) { return Result(std::move(value)); }

  [[nodiscard]] static Result failure(Status status) {
    if (status.is_ok()) {
      return Result(Status::internal("Result failure requires an error status"));
    }
    return Result(std::move(status));
  }

  [[nodiscard]] bool is_ok() const noexcept { return std::holds_alternative<T>(storage_); }

  [[nodiscard]] explicit operator bool() const noexcept { return is_ok(); }

  [[nodiscard]] T& value() & { return std::get<T>(storage_); }

  [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }

  [[nodiscard]] T&& value() && { return std::move(std::get<T>(storage_)); }

  [[nodiscard]] const Status& status() const& {
    if (is_ok()) {
      static const Status ok_status = Status::ok();
      return ok_status;
    }
    return std::get<Status>(storage_);
  }

private:
  explicit Result(T value) : storage_(std::in_place_type<T>, std::move(value)) {}

  explicit Result(Status status) : storage_(std::in_place_type<Status>, std::move(status)) {}

  std::variant<T, Status> storage_;
};

template <>
class Result<void> {
public:
  [[nodiscard]] static Result success() { return Result(Status::ok()); }

  [[nodiscard]] static Result failure(Status status) {
    if (status.is_ok()) {
      return Result(Status::internal("Result failure requires an error status"));
    }
    return Result(std::move(status));
  }

  [[nodiscard]] bool is_ok() const noexcept { return status_.is_ok(); }

  [[nodiscard]] explicit operator bool() const noexcept { return is_ok(); }

  [[nodiscard]] const Status& status() const& { return status_; }

private:
  explicit Result(Status status) : status_(std::move(status)) {}

  Status status_;
};

}  // namespace synarch
