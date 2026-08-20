#include <gtest/gtest.h>

#include <tinydb/bytes.h>
#include <tinydb/status.h>

#include "txn/contract.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

/*
** CONTRACT TESTS
**
** These tests pin small public scalar contracts: status names, size limits,
** and unsigned byte ordering. Transaction, recovery, and corruption behavior
** remains in the database and crash suites instead of being reproduced here.
*/

TEST(Contract, StatusNames) {
  EXPECT_EQ(tinydb::Status::Busy("x").ToString(), "busy: x");
  EXPECT_EQ(tinydb::Status::Corruption("x").ToString(), "corruption: x");
  EXPECT_EQ(tinydb::Status::IndeterminateCommit("x").ToString(), "indeterminate commit: x");
  EXPECT_EQ(tinydb::Status::NeedsRecovery("x").ToString(), "needs recovery: x");
}

TEST(Contract, Limits) {
  EXPECT_TRUE(tinydb::txn::ValidKeySize(0));
  EXPECT_TRUE(tinydb::txn::ValidKeySize(tinydb::MAX_KEY_BYTES));
  EXPECT_FALSE(tinydb::txn::ValidKeySize(tinydb::MAX_KEY_BYTES + 1U));
  EXPECT_TRUE(tinydb::txn::ValidValueSize(tinydb::MAX_VALUE_BYTES));
  EXPECT_FALSE(tinydb::txn::ValidValueSize(tinydb::MAX_VALUE_BYTES + 1U));
}

TEST(Contract, ByteOrder) {
  const auto less = tinydb::txn::BytewiseLess{};
  const auto low = std::string(1, static_cast<char>(0x7f));
  const auto high = std::string(1, static_cast<char>(0x80));
  EXPECT_TRUE(less("", "a"));
  EXPECT_TRUE(less(low, high));
  EXPECT_FALSE(less(high, low));
  EXPECT_TRUE(less(std::string{"a\0", 2}, std::string{"a\1", 2}));
  EXPECT_TRUE(less("prefix", "prefix-longer"));
  EXPECT_FALSE(less("prefix-longer", "prefix"));
  EXPECT_LT(tinydb::txn::BytewiseCompare(low, high), 0);
  EXPECT_EQ(tinydb::txn::BytewiseCompare(std::string{"a\0b", 3}, std::string{"a\0b", 3}), 0);
  EXPECT_GT(tinydb::txn::BytewiseCompare("prefix-longer", "prefix"), 0);

  const auto reference = [](std::string_view left, std::string_view right) {
    const auto common = std::min(left.size(), right.size());
    const auto order = common == 0 ? 0 : std::memcmp(left.data(), right.data(), common);
    return order != 0 ? order
                      : static_cast<int>(left.size() > right.size()) - static_cast<int>(left.size() < right.size());
  };
  for (auto size = std::size_t{0}; size <= 65; ++size) {
    auto left = std::string(size, '\0');
    for (auto index = std::size_t{0}; index < size; ++index) {
      left[index] = static_cast<char>((index * 37U + size) & 0xffU);
    }
    auto right = left;
    EXPECT_EQ(tinydb::txn::BytewiseCompare(left, right), 0);
    for (auto mismatch = std::size_t{0}; mismatch < size; ++mismatch) {
      right = left;
      right[mismatch] = static_cast<char>(static_cast<unsigned char>(right[mismatch]) ^ 0x80U);
      const auto expected = reference(left, right);
      const auto actual = tinydb::txn::BytewiseCompare(left, right);
      EXPECT_EQ((actual > 0) - (actual < 0), (expected > 0) - (expected < 0))
          << "size=" << size << " mismatch=" << mismatch;
    }
    right = left;
    right.push_back('\0');
    EXPECT_LT(tinydb::txn::BytewiseCompare(left, right), 0);
  }
}
