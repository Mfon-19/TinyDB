#include <gtest/gtest.h>
#include <tinydb/status.h>

#include "support/transaction_model.h"
#include "txn/state.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

using tinydb::StatusCode;
using tinydb::test_support::TransactionModel;
using tinydb::txn::CommitOutcome;
using tinydb::txn::DatabaseOperation;
using tinydb::txn::DatabaseState;
using tinydb::txn::TransactionState;

TEST(StatusContractTest, NewCodesHaveStableNames) {
  const auto cases = std::array{
      std::pair{tinydb::Status::Busy("held"), std::string_view{"busy: held"}},
      std::pair{tinydb::Status::UnsupportedFormat("version 9"), std::string_view{"unsupported format: version 9"}},
      std::pair{tinydb::Status::IndeterminateCommit("sync failed"),
                std::string_view{"indeterminate commit: sync failed"}},
      std::pair{tinydb::Status::NeedsRecovery("reopen"), std::string_view{"needs recovery: reopen"}},
  };

  for (const auto &[status, text] : cases) {
    EXPECT_EQ(status.ToString(), text);
  }
}

TEST(DatabaseStateTest, OnlyDefinedTransitionsAreLegal) {
  constexpr auto states = std::array{DatabaseState::Open, DatabaseState::CheckpointDegraded,
                                     DatabaseState::NeedsRecovery, DatabaseState::Corrupt, DatabaseState::Closed};
  constexpr auto allowed = std::array{
      std::pair{DatabaseState::Open, DatabaseState::CheckpointDegraded},
      std::pair{DatabaseState::Open, DatabaseState::NeedsRecovery},
      std::pair{DatabaseState::Open, DatabaseState::Corrupt},
      std::pair{DatabaseState::Open, DatabaseState::Closed},
      std::pair{DatabaseState::CheckpointDegraded, DatabaseState::Open},
      std::pair{DatabaseState::CheckpointDegraded, DatabaseState::NeedsRecovery},
      std::pair{DatabaseState::CheckpointDegraded, DatabaseState::Corrupt},
      std::pair{DatabaseState::CheckpointDegraded, DatabaseState::Closed},
      std::pair{DatabaseState::NeedsRecovery, DatabaseState::Closed},
      std::pair{DatabaseState::Corrupt, DatabaseState::Closed},
  };

  for (const auto from : states) {
    for (const auto to : states) {
      const auto expected = std::ranges::find(allowed, std::pair{from, to}) != allowed.end();
      SCOPED_TRACE("from=" + std::to_string(static_cast<int>(from)) + " to=" + std::to_string(static_cast<int>(to)));
      EXPECT_EQ(tinydb::txn::CanTransition(from, to), expected);
    }
  }
}

TEST(DatabaseStateTest, AdmissionPolicyDefinesEveryOperationInEveryState) {
  constexpr auto operations = std::array{
      DatabaseOperation::Read,   DatabaseOperation::Write, DatabaseOperation::Checkpoint, DatabaseOperation::Backup,
      DatabaseOperation::Verify, DatabaseOperation::Stats, DatabaseOperation::Close};
  constexpr auto cases = std::array{
      std::pair{DatabaseState::Open, std::array{StatusCode::Ok, StatusCode::Ok, StatusCode::Ok, StatusCode::Ok,
                                                StatusCode::Ok, StatusCode::Ok, StatusCode::Ok}},
      std::pair{DatabaseState::CheckpointDegraded,
                std::array{StatusCode::Ok, StatusCode::Ok, StatusCode::Ok, StatusCode::Ok, StatusCode::Ok,
                           StatusCode::Ok, StatusCode::Ok}},
      std::pair{DatabaseState::NeedsRecovery,
                std::array{StatusCode::NeedsRecovery, StatusCode::NeedsRecovery, StatusCode::NeedsRecovery,
                           StatusCode::NeedsRecovery, StatusCode::NeedsRecovery, StatusCode::Ok, StatusCode::Ok}},
      std::pair{DatabaseState::Corrupt,
                std::array{StatusCode::Corruption, StatusCode::Corruption, StatusCode::Corruption,
                           StatusCode::Corruption, StatusCode::Ok, StatusCode::Ok, StatusCode::Ok}},
      std::pair{DatabaseState::Closed,
                std::array{StatusCode::Closed, StatusCode::Closed, StatusCode::Closed, StatusCode::Closed,
                           StatusCode::Closed, StatusCode::Closed, StatusCode::Ok}},
  };

  for (const auto &[state, expected] : cases) {
    for (std::size_t i = 0; i < operations.size(); ++i) {
      SCOPED_TRACE("state=" + std::to_string(static_cast<int>(state)) + " operation=" + std::to_string(i));
      EXPECT_EQ(tinydb::txn::StateStatus(state, operations[i]), expected[i]);
    }
  }
}

TEST(TransactionStateTest, CommitPathAndFailurePathsAreExplicit) {
  constexpr auto states = std::array{
      TransactionState::Active,    TransactionState::Frozen,  TransactionState::WritingWal,   TransactionState::Durable,
      TransactionState::Published, TransactionState::Aborted, TransactionState::Indeterminate};
  constexpr auto allowed = std::array{
      std::pair{TransactionState::Active, TransactionState::Frozen},
      std::pair{TransactionState::Active, TransactionState::Aborted},
      std::pair{TransactionState::Frozen, TransactionState::WritingWal},
      std::pair{TransactionState::Frozen, TransactionState::Aborted},
      std::pair{TransactionState::WritingWal, TransactionState::Durable},
      std::pair{TransactionState::WritingWal, TransactionState::Aborted},
      std::pair{TransactionState::WritingWal, TransactionState::Indeterminate},
      std::pair{TransactionState::Durable, TransactionState::Published},
  };

  for (const auto from : states) {
    for (const auto to : states) {
      const auto expected = std::ranges::find(allowed, std::pair{from, to}) != allowed.end();
      SCOPED_TRACE("from=" + std::to_string(static_cast<int>(from)) + " to=" + std::to_string(static_cast<int>(to)));
      EXPECT_EQ(tinydb::txn::CanTransition(from, to), expected);
    }
  }
}

TEST(TransactionStateTest, OnlyTerminalStatesHaveCommitOutcomes) {
  EXPECT_EQ(tinydb::txn::Outcome(TransactionState::Published), CommitOutcome::Committed);
  EXPECT_EQ(tinydb::txn::Outcome(TransactionState::Aborted), CommitOutcome::Aborted);
  EXPECT_EQ(tinydb::txn::Outcome(TransactionState::Indeterminate), CommitOutcome::Indeterminate);
  EXPECT_EQ(tinydb::txn::Outcome(TransactionState::Active), std::nullopt);
  EXPECT_EQ(tinydb::txn::Outcome(TransactionState::Durable), std::nullopt);
}

TEST(TransactionModelTest, CommitPublishesAllChangesTogether) {
  auto model = TransactionModel{};
  auto transaction = model.BeginWrite();
  ASSERT_TRUE(transaction.has_value());

  transaction->Put("doc/1", "new contents");
  transaction->Put("tag/database/doc/1", "");

  EXPECT_EQ(transaction->Get("doc/1"), std::optional<std::string>{"new contents"});
  EXPECT_EQ(model.Get("doc/1"), std::nullopt);
  EXPECT_EQ(model.Get("tag/database/doc/1"), std::nullopt);

  ASSERT_TRUE(transaction->Commit());
  EXPECT_EQ(model.Get("doc/1"), std::optional<std::string>{"new contents"});
  EXPECT_EQ(model.Get("tag/database/doc/1"), std::optional<std::string>{""});
}

TEST(TransactionModelTest, AbortAndDestructionDiscardEveryChange) {
  auto model = TransactionModel{};
  {
    auto transaction = model.BeginWrite();
    ASSERT_TRUE(transaction.has_value());
    transaction->Put("key", "aborted");
    transaction->Abort();
  }
  EXPECT_EQ(model.Get("key"), std::nullopt);

  {
    auto transaction = model.BeginWrite();
    ASSERT_TRUE(transaction.has_value());
    transaction->Put("key", "also aborted");
  }
  EXPECT_EQ(model.Get("key"), std::nullopt);
  EXPECT_TRUE(model.BeginWrite().has_value());
}

TEST(TransactionModelTest, OverwriteDeleteAndReadYourWritesMatchTheContract) {
  auto model = TransactionModel{};
  auto first = model.BeginWrite();
  ASSERT_TRUE(first.has_value());
  first->Put("key", "first");
  ASSERT_TRUE(first->Commit());

  auto second = model.BeginWrite();
  ASSERT_TRUE(second.has_value());
  second->Put("key", "second");
  second->Delete("absent");
  EXPECT_EQ(second->Get("key"), std::optional<std::string>{"second"});
  second->Delete("key");
  EXPECT_EQ(second->Get("key"), std::nullopt);
  ASSERT_TRUE(second->Commit());
  EXPECT_EQ(model.Get("key"), std::nullopt);
}

TEST(TransactionModelTest, RangesAreHalfOpenAndMayBeUnbounded) {
  auto model = TransactionModel{};
  auto transaction = model.BeginWrite();
  ASSERT_TRUE(transaction.has_value());
  for (const auto *key : {"a", "b", "c", "d"}) {
    transaction->Put(key, key);
  }
  ASSERT_TRUE(transaction->Commit());

  EXPECT_EQ(model.Scan("b", "d"), (TransactionModel::Rows{{"b", "b"}, {"c", "c"}}));
  EXPECT_EQ(model.Scan(std::nullopt, "c"), (TransactionModel::Rows{{"a", "a"}, {"b", "b"}}));
  EXPECT_EQ(model.Scan("c", std::nullopt), (TransactionModel::Rows{{"c", "c"}, {"d", "d"}}));
  EXPECT_TRUE(model.Scan("c", "c").empty());
}

TEST(TransactionModelTest, KeysUseUnsignedLexicographicByteOrder) {
  auto model = TransactionModel{};
  auto transaction = model.BeginWrite();
  ASSERT_TRUE(transaction.has_value());
  transaction->Put(std::string{"\x80", 1}, "high");
  transaction->Put(std::string{"\x7f", 1}, "low");
  ASSERT_TRUE(transaction->Commit());

  const auto rows = model.Scan();
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0].first, (std::string{"\x7f", 1}));
  EXPECT_EQ(rows[1].first, (std::string{"\x80", 1}));
}

TEST(TransactionModelTest, OnlyOneWriterMayExist) {
  auto model = TransactionModel{};
  auto first = model.BeginWrite();
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(model.BeginWrite().has_value());
  first->Abort();
  EXPECT_TRUE(model.BeginWrite().has_value());
}

}  // namespace
