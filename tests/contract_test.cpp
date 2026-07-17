#include <gtest/gtest.h>

#include <tinydb/bytes.h>
#include <tinydb/status.h>

#include "txn/contract.h"
#include "txn/state.h"

#include <array>
#include <cstddef>
#include <string>

/*
** CONTRACT TESTS
**
** These tests pin the application-visible state machines and byte-string
** rules without reproducing storage behavior in a second model. End-to-end
** transaction, recovery, and corruption guarantees live in the database and
** crash suites.
*/

TEST(Contract, StatusNames) {
  EXPECT_EQ(tinydb::Status::Busy("x").ToString(), "busy: x");
  EXPECT_EQ(tinydb::Status::Corruption("x").ToString(), "corruption: x");
  EXPECT_EQ(tinydb::Status::IndeterminateCommit("x").ToString(), "indeterminate commit: x");
  EXPECT_EQ(tinydb::Status::NeedsRecovery("x").ToString(), "needs recovery: x");
}

TEST(Contract, DatabaseStates) {
  using enum tinydb::txn::DatabaseLifecycle;
  using enum tinydb::txn::DatabaseOperation;
  using tinydb::StatusCode;

  EXPECT_TRUE(tinydb::txn::CanTransition(Open, CheckpointDegraded));
  EXPECT_TRUE(tinydb::txn::CanTransition(Open, NeedsRecovery));
  EXPECT_TRUE(tinydb::txn::CanTransition(Open, Corrupt));
  EXPECT_TRUE(tinydb::txn::CanTransition(Open, Closed));
  EXPECT_FALSE(tinydb::txn::CanTransition(Closed, Open));

  EXPECT_EQ(tinydb::txn::StateStatus(Open, Write), StatusCode::Ok);
  EXPECT_EQ(tinydb::txn::StateStatus(CheckpointDegraded, Read), StatusCode::Ok);
  EXPECT_EQ(tinydb::txn::StateStatus(NeedsRecovery, Read), StatusCode::NeedsRecovery);
  EXPECT_EQ(tinydb::txn::StateStatus(NeedsRecovery, Stats), StatusCode::Ok);
  EXPECT_EQ(tinydb::txn::StateStatus(Corrupt, Verify), StatusCode::Ok);
  EXPECT_EQ(tinydb::txn::StateStatus(Corrupt, Write), StatusCode::Corruption);
  EXPECT_EQ(tinydb::txn::StateStatus(Closed, Close), StatusCode::Ok);
  EXPECT_EQ(tinydb::txn::StateStatus(Open, Close, 1), StatusCode::Busy);
}

TEST(Contract, TransactionStates) {
  using enum tinydb::txn::TransactionState;
  using tinydb::txn::CommitOutcome;

  EXPECT_TRUE(tinydb::txn::CanTransition(Active, Frozen));
  EXPECT_TRUE(tinydb::txn::CanTransition(Frozen, WritingWal));
  EXPECT_TRUE(tinydb::txn::CanTransition(WritingWal, Durable));
  EXPECT_TRUE(tinydb::txn::CanTransition(Durable, Published));
  EXPECT_TRUE(tinydb::txn::CanTransition(WritingWal, Indeterminate));
  EXPECT_FALSE(tinydb::txn::CanTransition(Durable, Aborted));
  EXPECT_EQ(tinydb::txn::Outcome(Published), CommitOutcome::Committed);
  EXPECT_EQ(tinydb::txn::Outcome(Aborted), CommitOutcome::DefinitelyAborted);
  EXPECT_EQ(tinydb::txn::Outcome(Indeterminate), CommitOutcome::Indeterminate);
  EXPECT_FALSE(tinydb::txn::Outcome(Active).has_value());
}

TEST(Contract, Limits) {
  EXPECT_EQ(tinydb::txn::ValidateKeySize(0), tinydb::StatusCode::Ok);
  EXPECT_EQ(tinydb::txn::ValidateKeySize(tinydb::MAX_KEY_BYTES), tinydb::StatusCode::Ok);
  EXPECT_EQ(tinydb::txn::ValidateKeySize(tinydb::MAX_KEY_BYTES + 1U), tinydb::StatusCode::InvalidArgument);
  EXPECT_EQ(tinydb::txn::ValidateValueSize(tinydb::MAX_VALUE_BYTES), tinydb::StatusCode::Ok);
  EXPECT_EQ(tinydb::txn::ValidateValueSize(tinydb::MAX_VALUE_BYTES + 1U), tinydb::StatusCode::InvalidArgument);
}

TEST(Contract, ByteOrder) {
  const auto less = tinydb::txn::BytewiseLess{};
  const auto low = std::string(1, static_cast<char>(0x7f));
  const auto high = std::string(1, static_cast<char>(0x80));
  EXPECT_TRUE(less("", "a"));
  EXPECT_TRUE(less(low, high));
  EXPECT_FALSE(less(high, low));
}
