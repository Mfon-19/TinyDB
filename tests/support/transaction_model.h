#pragma once

#include "txn/contract.h"

#include <tinydb/status.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydb::test_support {

class TransactionModel {
 public:
  using Row = std::pair<std::string, std::string>;
  using Rows = std::vector<Row>;

  TransactionModel() = default;
  TransactionModel(const TransactionModel &) = delete;
  auto operator=(const TransactionModel &) -> TransactionModel & = delete;
  TransactionModel(TransactionModel &&) = delete;
  auto operator=(TransactionModel &&) -> TransactionModel & = delete;

  class WriteTransaction {
   public:
    WriteTransaction(const WriteTransaction &) = delete;
    auto operator=(const WriteTransaction &) -> WriteTransaction & = delete;

    WriteTransaction(WriteTransaction &&other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), working_(std::move(other.working_)) {}

    auto operator=(WriteTransaction &&) -> WriteTransaction & = delete;

    ~WriteTransaction() { Abort(); }

    auto Get(std::string_view key) const -> std::optional<std::string> { return Find(working_, key); }

    [[nodiscard]] auto Put(std::string key, std::string value) -> StatusCode {
      if (const auto status = txn::ValidateKeySize(key.size()); status != StatusCode::Ok) {
        return status;
      }
      working_.insert_or_assign(std::move(key), std::move(value));
      return StatusCode::Ok;
    }

    [[nodiscard]] auto Delete(std::string_view key) -> StatusCode {
      if (const auto status = txn::ValidateKeySize(key.size()); status != StatusCode::Ok) {
        return status;
      }
      if (const auto found = working_.find(key); found != working_.end()) {
        working_.erase(found);
      }
      return StatusCode::Ok;
    }

    auto Scan(std::optional<std::string_view> start = std::nullopt,
              std::optional<std::string_view> end = std::nullopt) const -> Rows {
      return TransactionModel::ScanRows(working_, start, end);
    }

    auto Commit() -> bool {
      if (owner_ == nullptr) {
        return false;
      }
      owner_->committed_ = std::move(working_);
      Release();
      return true;
    }

    void Abort() noexcept {
      if (owner_ != nullptr) {
        Release();
      }
    }

   private:
    friend class TransactionModel;

    explicit WriteTransaction(TransactionModel &owner) : owner_(&owner), working_(owner.committed_) {}

    static auto Find(const std::map<std::string, std::string, txn::BytewiseLess> &rows,
                     std::string_view key) -> std::optional<std::string> {
      const auto found = rows.find(key);
      return found == rows.end() ? std::nullopt : std::optional<std::string>{found->second};
    }

    void Release() noexcept {
      owner_->writer_active_ = false;
      owner_ = nullptr;
    }

    TransactionModel *owner_;
    std::map<std::string, std::string, txn::BytewiseLess> working_;
  };

  auto BeginWrite() -> std::optional<WriteTransaction> {
    if (writer_active_) {
      return std::nullopt;
    }
    writer_active_ = true;
    return WriteTransaction{*this};
  }

  auto Get(std::string_view key) const -> std::optional<std::string> {
    const auto found = committed_.find(key);
    return found == committed_.end() ? std::nullopt : std::optional<std::string>{found->second};
  }

  auto Scan(std::optional<std::string_view> start = std::nullopt,
            std::optional<std::string_view> end = std::nullopt) const -> Rows {
    return ScanRows(committed_, start, end);
  }

 private:
  static auto ScanRows(const std::map<std::string, std::string, txn::BytewiseLess> &rows,
                       std::optional<std::string_view> start, std::optional<std::string_view> end) -> Rows {
    auto result = Rows{};
    auto current = start ? rows.lower_bound(*start) : rows.begin();
    while (current != rows.end() && (!end || txn::BytewiseLess{}(current->first, *end))) {
      result.emplace_back(current->first, current->second);
      ++current;
    }
    return result;
  }

  std::map<std::string, std::string, txn::BytewiseLess> committed_;
  bool writer_active_{false};
};

}  // namespace tinydb::test_support
