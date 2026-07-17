#include <gtest/gtest.h>
#include <tinydb/status.h>

#include "support/transaction_model.h"
#include "support/transaction_scenarios.h"
#include "txn/contract.h"
#include "txn/state.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

/*
** These tests are executable product contracts rather than implementation
** tests. They enumerate every legal lifecycle and transaction transition and
** run reusable key/value scenarios against a reference transaction model.
** When the real public transaction API arrives, the same scenarios are run
** against TinyDB without changing their expected semantics.
*/
namespace {

using tinydb::StatusCode;
using tinydb::test_support::TransactionModel;
using tinydb::txn::CommitOutcome;
using tinydb::txn::DatabaseOperation;
using tinydb::txn::DatabaseLifecycle;
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
  constexpr auto states = std::array{DatabaseLifecycle::Open, DatabaseLifecycle::CheckpointDegraded,
                                     DatabaseLifecycle::NeedsRecovery, DatabaseLifecycle::Corrupt,
                                     DatabaseLifecycle::Closed};
  constexpr auto allowed = std::array{
      std::pair{DatabaseLifecycle::Open, DatabaseLifecycle::CheckpointDegraded},
      std::pair{DatabaseLifecycle::Open, DatabaseLifecycle::NeedsRecovery},
      std::pair{DatabaseLifecycle::Open, DatabaseLifecycle::Corrupt},
      std::pair{DatabaseLifecycle::Open, DatabaseLifecycle::Closed},
      std::pair{DatabaseLifecycle::CheckpointDegraded, DatabaseLifecycle::Open},
      std::pair{DatabaseLifecycle::CheckpointDegraded, DatabaseLifecycle::NeedsRecovery},
      std::pair{DatabaseLifecycle::CheckpointDegraded, DatabaseLifecycle::Corrupt},
      std::pair{DatabaseLifecycle::CheckpointDegraded, DatabaseLifecycle::Closed},
      std::pair{DatabaseLifecycle::NeedsRecovery, DatabaseLifecycle::Closed},
      std::pair{DatabaseLifecycle::Corrupt, DatabaseLifecycle::Closed},
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
      DatabaseOperation::Read, DatabaseOperation::Write, DatabaseOperation::Checkpoint,
      DatabaseOperation::Verify, DatabaseOperation::Stats, DatabaseOperation::Close};
  constexpr auto cases = std::array{
      std::pair{DatabaseLifecycle::Open, std::array{StatusCode::Ok, StatusCode::Ok, StatusCode::Ok, StatusCode::Ok,
                                                   StatusCode::Ok, StatusCode::Ok}},
      std::pair{DatabaseLifecycle::CheckpointDegraded,
                std::array{StatusCode::Ok, StatusCode::Ok, StatusCode::Ok, StatusCode::Ok, StatusCode::Ok,
                           StatusCode::Ok}},
      std::pair{DatabaseLifecycle::NeedsRecovery,
                std::array{StatusCode::NeedsRecovery, StatusCode::NeedsRecovery, StatusCode::NeedsRecovery,
                           StatusCode::NeedsRecovery, StatusCode::Ok, StatusCode::Ok}},
      std::pair{DatabaseLifecycle::Corrupt,
                std::array{StatusCode::Corruption, StatusCode::Corruption, StatusCode::Corruption, StatusCode::Ok,
                           StatusCode::Ok, StatusCode::Ok}},
      std::pair{DatabaseLifecycle::Closed,
                std::array{StatusCode::Closed, StatusCode::Closed, StatusCode::Closed, StatusCode::Closed,
                           StatusCode::Closed, StatusCode::Ok}},
  };

  for (const auto &[state, expected] : cases) {
    for (std::size_t i = 0; i < operations.size(); ++i) {
      SCOPED_TRACE("state=" + std::to_string(static_cast<int>(state)) + " operation=" + std::to_string(i));
      EXPECT_EQ(tinydb::txn::StateStatus(state, operations[i]), expected[i]);
    }
  }
}

TEST(DatabaseStateTest, CloseIsBusyUntilEveryTransactionReleasesItsSnapshot) {
  for (const auto state :
       {DatabaseLifecycle::Open, DatabaseLifecycle::CheckpointDegraded, DatabaseLifecycle::NeedsRecovery,
        DatabaseLifecycle::Corrupt}) {
    EXPECT_EQ(tinydb::txn::StateStatus(state, DatabaseOperation::Close, 1), StatusCode::Busy);
    EXPECT_EQ(tinydb::txn::StateStatus(state, DatabaseOperation::Close, 2), StatusCode::Busy);
    EXPECT_EQ(tinydb::txn::StateStatus(state, DatabaseOperation::Close, 0), StatusCode::Ok);
  }

  EXPECT_EQ(tinydb::txn::StateStatus(DatabaseLifecycle::Closed, DatabaseOperation::Close, 0), StatusCode::Ok);
}

TEST(DatabaseStateTest, OpeningAnOwnedDatabaseIsBusy) {
  EXPECT_EQ(tinydb::txn::OpenStatus(false), StatusCode::Ok);
  EXPECT_EQ(tinydb::txn::OpenStatus(true), StatusCode::Busy);
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
  EXPECT_EQ(tinydb::txn::Outcome(TransactionState::Aborted), CommitOutcome::DefinitelyAborted);
  EXPECT_EQ(tinydb::txn::Outcome(TransactionState::Indeterminate), CommitOutcome::Indeterminate);
  for (const auto state :
       {TransactionState::Active, TransactionState::Frozen, TransactionState::WritingWal, TransactionState::Durable}) {
    EXPECT_EQ(tinydb::txn::Outcome(state), std::nullopt);
  }
}

TEST(TransactionContractTest, CommitPublishesAllChangesTogether) {
  auto model = TransactionModel{};
  tinydb::test_support::CommitPublishesAtomically(model);
}

TEST(TransactionContractTest, AbortDiscardsEveryChange) {
  auto model = TransactionModel{};
  tinydb::test_support::AbortDiscardsAllChanges(model);
}

TEST(TransactionContractTest, DestructionAbortsAndReleasesTheWriter) {
  auto model = TransactionModel{};
  tinydb::test_support::DestructionAborts(model);
}

TEST(TransactionContractTest, OverwriteDeleteAndReadYourWritesMatchTheContract) {
  auto model = TransactionModel{};
  tinydb::test_support::OverwriteDeleteAndReadOwnWrites(model);
}

TEST(TransactionContractTest, RangesAreHalfOpenAndMayBeUnbounded) {
  auto model = TransactionModel{};
  tinydb::test_support::ScanUsesHalfOpenOptionalBounds(model);
}

TEST(TransactionContractTest, KeysUseUnsignedLexicographicByteOrder) {
  auto model = TransactionModel{};
  tinydb::test_support::KeysUseUnsignedByteOrder(model);
}

TEST(TransactionContractTest, OnlyOneWriterMayExist) {
  auto model = TransactionModel{};
  tinydb::test_support::OnlyOneWriterIsAdmitted(model);
}

TEST(DataModelContractTest, EmptyAndMaximumSizedKeysAreValid) {
  EXPECT_EQ(tinydb::txn::ValidateKeySize(0), StatusCode::Ok);
  EXPECT_EQ(tinydb::txn::ValidateKeySize(tinydb::MAX_KEY_BYTES), StatusCode::Ok);
  EXPECT_EQ(tinydb::txn::ValidateKeySize(tinydb::MAX_KEY_BYTES + 1), StatusCode::InvalidArgument);

  auto model = TransactionModel{};
  auto transaction = model.BeginWrite();
  ASSERT_TRUE(transaction.has_value());
  const auto maximum_key = std::string(tinydb::MAX_KEY_BYTES, 'k');
  ASSERT_EQ(transaction->Put("", ""), StatusCode::Ok);
  ASSERT_EQ(transaction->Put(maximum_key, "maximum"), StatusCode::Ok);
  ASSERT_TRUE(transaction->Commit());
  EXPECT_EQ(model.Get(""), std::optional<std::string>{""});
  EXPECT_EQ(model.Get(maximum_key), std::optional<std::string>{"maximum"});
}

TEST(DataModelContractTest, InvalidMutationDoesNotAbortTheTransaction) {
  auto model = TransactionModel{};
  auto transaction = model.BeginWrite();
  ASSERT_TRUE(transaction.has_value());

  const auto oversized_key = std::string(tinydb::MAX_KEY_BYTES + 1, 'x');
  EXPECT_EQ(transaction->Put(oversized_key, "value"), StatusCode::InvalidArgument);
  EXPECT_EQ(transaction->Delete(oversized_key), StatusCode::InvalidArgument);
  EXPECT_EQ(transaction->Put("valid", "value"), StatusCode::Ok);
  ASSERT_TRUE(transaction->Commit());

  EXPECT_EQ(model.Get(oversized_key), std::nullopt);
  EXPECT_EQ(model.Get("valid"), std::optional<std::string>{"value"});
}

TEST(DataModelContractTest, ValuesAreNotLimitedByPageGeometry) {
  EXPECT_EQ(tinydb::txn::ValidateValueSize(tinydb::MAX_VALUE_BYTES), StatusCode::Ok);
  EXPECT_EQ(tinydb::txn::ValidateValueSize(tinydb::MAX_VALUE_BYTES + 1), StatusCode::InvalidArgument);

  auto model = TransactionModel{};
  auto transaction = model.BeginWrite();
  ASSERT_TRUE(transaction.has_value());

  const auto value = std::string(16 * 1024, 'v');
  ASSERT_EQ(transaction->Put("large", value), StatusCode::Ok);
  ASSERT_TRUE(transaction->Commit());
  EXPECT_EQ(model.Get("large"), std::optional<std::string>{value});
}

}  // namespace
