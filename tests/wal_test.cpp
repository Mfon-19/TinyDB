#include "tinydb/storage/disk_manager.h"
#include "tinydb/storage/encoding.h"
#include "tinydb/storage/page_codec.h"
#include "tinydb/storage/wal.h"
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace tinydb::storage {
namespace {

auto MakePage(PageId page_id, std::string_view value) -> std::shared_ptr<Page> {
  const LeafEntry entry{"key", value};
  return std::make_shared<Page>(
      EncodeLeafPage(page_id, INVALID_PAGE_ID, std::span{&entry, 1}).value());
}

auto ValuePages(const PageMap &pages) -> std::map<PageId, Page> {
  std::map<PageId, Page> values;
  for (const auto &[id, page] : pages) {
    values.emplace(id, *page);
  }
  return values;
}

void Append(std::vector<char> &bytes, std::span<const char> record) {
  bytes.insert(bytes.end(), record.begin(), record.end());
}

constexpr std::uint32_t SALT = 0x5a17;

auto Header() -> std::vector<char> {
  const auto header = EncodeWalHeader(SALT);
  return {header.begin(), header.end()};
}

auto Record(const PageMap &pages) -> std::vector<char> {
  return EncodeWalRecord(pages, SALT).value();
}

TEST(WalCodec, RecordsRoundTrip) {
  const PageMap first{{1, MakePage(1, "root")}, {7, MakePage(7, "old")}};
  const auto record = Record(first);
  EXPECT_EQ(record.size(), 8212U);
  EXPECT_EQ(little_endian::GetU32(record, 0), SALT);
  EXPECT_EQ(little_endian::GetU32(record, 4), 2U);
  EXPECT_EQ(little_endian::GetU32(record, 8), 1U);
  EXPECT_EQ(little_endian::GetU32(record, 4108), 7U);
  auto bytes = Header();
  EXPECT_EQ(std::string_view(bytes.data(), 4), "TDW3");
  Append(bytes, record);
  EXPECT_EQ(ValuePages(DecodeWal(bytes).value()), ValuePages(first));

  const PageMap second{{7, MakePage(7, "new")}, {9, MakePage(9, "added")}};
  Append(bytes, Record(second));
  const PageMap expected{
      {1, first.at(1)}, {7, second.at(7)}, {9, second.at(9)}};
  EXPECT_EQ(ValuePages(DecodeWal(bytes).value()), ValuePages(expected));
  EXPECT_TRUE(DecodeWal({}).value().empty());
  EXPECT_TRUE(DecodeWal(Header()).value().empty());
}

TEST(WalCodec, IgnoresTornTail) {
  const PageMap first{{1, MakePage(1, "old")}};
  auto prefix = Header();
  Append(prefix, Record(first));
  auto final = Record({{1, MakePage(1, "new")}, {2, MakePage(2, "added")}});
  for (const std::size_t length : {0U, 1U, 7U, 8U, 12U, 4108U, 8208U, 8211U}) {
    SCOPED_TRACE(length);
    auto bytes = prefix;
    Append(bytes, std::span<const char>{final}.first(length));
    auto decoded = DecodeWal(bytes);
    ASSERT_TRUE(decoded) << decoded.error().Message();
    EXPECT_EQ(ValuePages(*decoded), ValuePages(first));
  }
  final.back() ^= 1;
  auto bytes = prefix;
  Append(bytes, final);
  EXPECT_EQ(ValuePages(DecodeWal(bytes).value()), ValuePages(first));
  bytes = prefix;
  Append(bytes, EncodeWalRecord(first, SALT + 1).value());
  EXPECT_EQ(ValuePages(DecodeWal(bytes).value()), ValuePages(first));
}

TEST(WalCodec, RejectsCorruption) {
  auto good = Header();
  Append(good, Record({{1, MakePage(1, "value")}}));
  std::vector<std::vector<char>> malformed;
  for (const std::size_t index : {0U, 4U, 8U}) {
    auto bad = good;
    bad[index] ^= 1;
    malformed.push_back(bad);
  }
  malformed.emplace_back(good.begin(), good.begin() + WAL_HEADER_SIZE - 1);
  for (const auto &bytes : malformed) {
    EXPECT_FALSE(DecodeWal(bytes));
  }

  const PageMap first{{1, MakePage(1, "value")}};
  const auto record = Record({{2, MakePage(2, "next")}});
  const auto damaged = [&](std::size_t offset, auto change) {
    auto bytes = Header();
    Append(bytes, Record(first));
    const auto start = bytes.size();
    Append(bytes, record);
    change(std::span<char>{bytes}.subspan(start + offset));
    return bytes;
  };
  std::vector<std::vector<char>> torn;
  for (const PageId page_id : {0U, 3U, INVALID_PAGE_ID}) {
    torn.push_back(damaged(8, [&](std::span<char> bytes) {
      little_endian::PutU32(bytes, 0, page_id);
    }));
  }
  for (const std::size_t offset :
       {std::size_t{12}, std::size_t{4100}, record.size() - 1}) {
    torn.push_back(
        damaged(offset, [](std::span<char> bytes) { bytes[0] ^= 1; }));
  }
  for (const auto &bytes : torn) {
    EXPECT_EQ(ValuePages(DecodeWal(bytes).value()), ValuePages(first));
  }
  EXPECT_FALSE(EncodeWalRecord({}, SALT));
  EXPECT_FALSE(EncodeWalRecord({{0, MakePage(1, "value")}}, SALT));
  EXPECT_FALSE(
      EncodeWalRecord({{INVALID_PAGE_ID, MakePage(1, "value")}}, SALT));
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

  std::string directory_;
  std::string path_;
  std::optional<DiskManager> disk_;
};

TEST_F(WalTest, AppendsAndResets) {
  const PageMap first{{1, MakePage(1, "old")}, {2, MakePage(2, "kept")}};
  const PageMap second{{1, MakePage(1, "new")}};
  {
    auto wal = Wal::Open(path_).value();
    EXPECT_TRUE(wal.Validate().value().empty());
    ASSERT_TRUE(wal.Reset().Ok());
    ASSERT_TRUE(wal.Append(EncodeWalRecord(first, wal.Salt()).value()).Ok());
    ASSERT_TRUE(wal.Sync().Ok());
    ASSERT_TRUE(wal.Append(EncodeWalRecord(second, wal.Salt()).value()).Ok());
    ASSERT_TRUE(wal.Sync().Ok());
  }
  auto wal = Wal::Open(path_).value();
  const PageMap expected{{1, second.at(1)}, {2, first.at(2)}};
  EXPECT_EQ(ValuePages(wal.Validate().value()), ValuePages(expected));
  const auto size = std::filesystem::file_size(path_ + "-wal");
  ASSERT_TRUE(wal.Reset().Ok());
  EXPECT_TRUE(wal.Validate().value().empty());
  ASSERT_TRUE(wal.Append(EncodeWalRecord(second, wal.Salt()).value()).Ok());
  ASSERT_TRUE(wal.Sync().Ok());
  EXPECT_EQ(ValuePages(wal.Validate().value()), ValuePages(second));
  EXPECT_EQ(std::filesystem::file_size(path_ + "-wal"), size);
}
}
}
