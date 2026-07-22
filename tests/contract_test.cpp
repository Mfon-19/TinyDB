#include <gtest/gtest.h>

#include <tinydb/bytes.h>
#include <tinydb/status.h>

#include "txn/contract.h"

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
