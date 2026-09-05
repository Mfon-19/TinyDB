#include "tinydb/storage/crc32.h"
#include "tinydb/storage/disk_manager.h"
#include "tinydb/storage/encoding.h"
#include "tinydb/storage/page_codec.h"
#include "tinydb/storage/wal.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <optional>
#include <string>

namespace tinydb::storage {
namespace {

auto MakePage(PageId page_id, std::string_view value) -> storage::Page {
  const LeafEntry entry{"key", value};
  return EncodeLeafPage(page_id, INVALID_PAGE_ID, std::span{&entry, 1}).value();
}

void Append(std::vector<char> &bytes, std::span<const char> record) {
  bytes.insert(bytes.end(), record.begin(), record.end());
}

TEST(WalCodec, RecordsRoundTrip) {
  const PageMap first{{1, MakePage(1, "root")}, {7, MakePage(7, "old")}};
  auto bytes = EncodeWalRecord(first).value();
  EXPECT_EQ(bytes.size(), 8212U);
  EXPECT_EQ(std::string_view(bytes.data(), 4), "TDW1");
  EXPECT_EQ(little_endian::GetU32(bytes, 4), 2U);
  EXPECT_EQ(little_endian::GetU32(bytes, 8), 1U);
  EXPECT_EQ(little_endian::GetU32(bytes, 4108), 7U);
  EXPECT_EQ(DecodeWal(bytes).value(), first);

  const PageMap second{{7, MakePage(7, "new")}, {9, MakePage(9, "added")}};
  Append(bytes, EncodeWalRecord(second).value());
  const PageMap expected{
      {1, first.at(1)}, {7, second.at(7)}, {9, second.at(9)}};
  EXPECT_EQ(DecodeWal(bytes).value(), expected);
  EXPECT_TRUE(DecodeWal({}).value().empty());
}

TEST(WalCodec, IgnoresTornTail) {
  const PageMap first{{1, MakePage(1, "old")}};
  const auto prefix = EncodeWalRecord(first).value();
  auto final =
      EncodeWalRecord({{1, MakePage(1, "new")}, {2, MakePage(2, "added")}})
          .value();
  for (const std::size_t length : {0U, 1U, 7U, 8U, 12U, 4108U, 8208U, 8211U}) {
    SCOPED_TRACE(length);
    auto bytes = prefix;
    Append(bytes, std::span<const char>{final}.first(length));
    auto decoded = DecodeWal(bytes);
    ASSERT_TRUE(decoded) << decoded.error().Message();
    EXPECT_EQ(*decoded, first);
  }
  final.back() ^= 1;
  auto bytes = prefix;
  Append(bytes, final);
  EXPECT_EQ(DecodeWal(bytes).value(), first);
  EXPECT_TRUE(DecodeWal(final).value().empty());
}

TEST(WalCodec, RejectsCorruption) {
  const auto good = EncodeWalRecord({{1, MakePage(1, "value")}}).value();
  std::vector<std::vector<char>> malformed;
  auto bad = good;
  bad[0] = 'X';
  malformed.push_back(bad);
  bad = good;
  little_endian::PutU32(bad, 4, 0);
  malformed.push_back(bad);
  for (const PageId page_id : {0U, INVALID_PAGE_ID}) {
    bad = good;
    little_endian::PutU32(bad, 8, page_id);
    little_endian::PutU32(
        bad, bad.size() - 4,
        Crc32(std::span<const char>{bad}.first(bad.size() - 4)));
    malformed.push_back(bad);
    bad.back() ^= 1;
    malformed.push_back(bad);
  }
  bad = good;
  bad.back() ^= 1;
  bad.push_back('x');
  malformed.push_back(bad);
  bad = good;
  bad[12] ^= 1;
  Append(bad, good);
  malformed.push_back(bad);

  for (const auto &record : malformed) {
    auto bytes = good;
    Append(bytes, record);
    EXPECT_FALSE(DecodeWal(bytes));
  }
  EXPECT_FALSE(EncodeWalRecord({}));
  EXPECT_FALSE(EncodeWalRecord({{0, MakePage(1, "value")}}));
  EXPECT_FALSE(EncodeWalRecord({{INVALID_PAGE_ID, MakePage(1, "value")}}));
}

class WalTest : public testing::Test {
protected:
  void SetUp() override {
    directory_ = testing::TempDir() + "tinydb_wal_XXXXXX";
    ASSERT_NE(mkdtemp(directory_.data()), nullptr);
    path_ = directory_ + "/database";
    disk_.emplace(DiskManager::Open(path_).value());
  }

  void TearDown() override {
    disk_.reset();
    std::filesystem::remove_all(directory_);
  }

  auto ReadWal() -> std::vector<char> {
    std::ifstream file(path_ + "-wal", std::ios::binary);
    return {std::istreambuf_iterator<char>{file}, {}};
  }

  std::string directory_;
  std::string path_;
  std::optional<DiskManager> disk_;
};

TEST_F(WalTest, AppendsAndResets) {
  const PageMap first{{1, MakePage(1, "old")}, {2, MakePage(2, "kept")}};
  const PageMap second{{1, MakePage(1, "new")}};
  {
    auto wal = Wal::Open(path_).value();
    EXPECT_TRUE(wal.Empty());
    EXPECT_TRUE(wal.Validate().value().empty());
    ASSERT_TRUE(wal.Append(EncodeWalRecord(first).value()).Ok());
    ASSERT_TRUE(wal.Sync().Ok());
    ASSERT_TRUE(wal.Append(EncodeWalRecord(second).value()).Ok());
    ASSERT_TRUE(wal.Sync().Ok());
  }
  auto wal = Wal::Open(path_).value();
  EXPECT_FALSE(wal.Empty());
  const PageMap expected{{1, second.at(1)}, {2, first.at(2)}};
  EXPECT_EQ(wal.Validate().value(), expected);
  ASSERT_TRUE(wal.Reset().Ok());
  EXPECT_TRUE(wal.Empty());
  EXPECT_EQ(std::filesystem::file_size(path_ + "-wal"), 0U);
  EXPECT_TRUE(wal.Validate().value().empty());
  ASSERT_TRUE(wal.Append(EncodeWalRecord(second).value()).Ok());
  ASSERT_TRUE(wal.Sync().Ok());
  EXPECT_EQ(wal.Validate().value(), second);
  EXPECT_EQ(ReadWal(), EncodeWalRecord(second).value());
}
}
}
