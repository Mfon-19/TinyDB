#include <gtest/gtest.h>

#include <tinydb/database.h>

#include "salvage/salvage.h"
#include "storage/page_codec.h"
#include "wal/wal.h"

#include <unistd.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

/*
** Salvage tests intentionally damage checkpointed source bytes.  Normal open
** must reject that file; the separate scanner may skip the damaged leaf and
** construct a new, independently verified database from surviving records.
*/
namespace {

auto ReadFile(const std::filesystem::path &path) -> std::vector<char> {
  auto input = std::ifstream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

class SalvageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto suffix = std::to_string(::getpid());
    source_ = std::filesystem::temp_directory_path() / ("tinydb_salvage_source_" + suffix + ".db");
    destination_ = std::filesystem::temp_directory_path() / ("tinydb_salvage_destination_" + suffix + ".db");
    Remove(source_);
    Remove(destination_);
  }

  void TearDown() override {
    Remove(source_);
    Remove(destination_);
  }

  static void Remove(const std::filesystem::path &path) {
    std::filesystem::remove(path);
    std::filesystem::remove(tinydb::Wal::PathFor(path));
  }

  auto LeafPages() const -> std::vector<tinydb::page_id_t> {
    auto input = std::ifstream(source_, std::ios::binary);
    input.seekg(0, std::ios::end);
    const auto bytes = static_cast<std::size_t>(input.tellg());
    auto result = std::vector<tinydb::page_id_t>{};
    auto page = std::array<char, tinydb::PAGE_SIZE>{};
    for (tinydb::page_id_t page_id = tinydb::FIRST_DATA_PAGE_ID; page_id < bytes / tinydb::PAGE_SIZE; ++page_id) {
      input.seekg(static_cast<std::streamoff>(page_id * tinydb::PAGE_SIZE));
      input.read(page.data(), static_cast<std::streamsize>(page.size()));
      const auto header = tinydb::storage::DecodeDataPageHeader(std::as_bytes(std::span{page}), page_id);
      if (header && header->type == tinydb::storage::DataPageType::Leaf) {
        result.push_back(page_id);
      }
    }
    return result;
  }

  std::filesystem::path source_;
  std::filesystem::path destination_;
};

TEST_F(SalvageTest, HealthyInputRoundTripsThroughANewDatabase) {
  const auto large = std::string(3U * tinydb::PAGE_SIZE, 'o');
  {
    auto source = tinydb::Database::Open(source_).value();
    ASSERT_TRUE(source.Put(std::string{"binary\0key", 10}, std::string{"value\0bytes", 11}).Ok());
    ASSERT_TRUE(source.Put("overflow", large).Ok());
    ASSERT_TRUE(source.Close().Ok());
  }

  const auto salvaged = tinydb::salvage::Run(source_, destination_);
  ASSERT_TRUE(salvaged.has_value()) << salvaged.error().ToString();
  EXPECT_EQ(salvaged->rows_recovered, 2U);
  EXPECT_TRUE(salvaged->allocator_filter_available);

  auto destination = tinydb::Database::Open(destination_).value();
  EXPECT_EQ(destination.Get(std::string{"binary\0key", 10}).value(), std::string("value\0bytes", 11));
  EXPECT_EQ(destination.Get("overflow").value(), large);
  EXPECT_TRUE(destination.Verify()->Ok());
}

TEST_F(SalvageTest, DamagedLeafIsSkippedWithoutChangingTheSource) {
  {
    auto source = tinydb::Database::Open(source_).value();
    for (std::size_t index = 0; index < 500; ++index) {
      ASSERT_TRUE(source.Put("key-" + std::to_string(index), std::string(200, 'v')).Ok());
    }
    ASSERT_TRUE(source.Close().Ok());
  }
  const auto leaf_pages = LeafPages();
  ASSERT_GT(leaf_pages.size(), 1U);

  const auto damaged_offset = static_cast<std::streamoff>(leaf_pages.front() * tinydb::PAGE_SIZE + 100U);
  {
    auto file = std::fstream(source_, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(damaged_offset);
    char byte = 0;
    file.read(&byte, 1);
    byte ^= 0x01;
    file.seekp(damaged_offset);
    file.write(&byte, 1);
  }
  const auto normal_open = tinydb::Database::Open(source_);
  ASSERT_FALSE(normal_open.has_value());
  EXPECT_EQ(normal_open.error().Code(), tinydb::StatusCode::Corruption);
  const auto damaged_source = ReadFile(source_);

  const auto salvaged = tinydb::salvage::Run(source_, destination_);
  ASSERT_TRUE(salvaged.has_value()) << salvaged.error().ToString();
  EXPECT_GT(salvaged->damaged_pages, 0U);
  EXPECT_GT(salvaged->rows_recovered, 0U);
  EXPECT_LT(salvaged->rows_recovered, 500U);
  EXPECT_EQ(ReadFile(source_), damaged_source);

  auto destination = tinydb::Database::Open(destination_).value();
  EXPECT_TRUE(destination.Verify()->Ok());
}

TEST_F(SalvageTest, LocallyValidLeavesSurviveLossOfBothSuperblocks) {
  {
    auto source = tinydb::Database::Open(source_).value();
    ASSERT_TRUE(source.Put("alpha", "one").Ok());
    ASSERT_TRUE(source.Put("omega", "two").Ok());
    ASSERT_TRUE(source.Close().Ok());
  }
  {
    auto file = std::fstream(source_, std::ios::in | std::ios::out | std::ios::binary);
    for (const auto page_id : {tinydb::SUPERBLOCK_A_PAGE_ID, tinydb::SUPERBLOCK_B_PAGE_ID}) {
      const auto offset = static_cast<std::streamoff>(page_id * tinydb::PAGE_SIZE);
      file.seekg(offset);
      char byte = 0;
      file.read(&byte, 1);
      byte ^= 0x01;
      file.seekp(offset);
      file.write(&byte, 1);
    }
  }

  const auto salvaged = tinydb::salvage::Run(source_, destination_);
  ASSERT_TRUE(salvaged.has_value()) << salvaged.error().ToString();
  EXPECT_FALSE(salvaged->superblock_available);
  EXPECT_FALSE(salvaged->allocator_filter_available);
  EXPECT_EQ(salvaged->rows_recovered, 2U);

  auto destination = tinydb::Database::Open(destination_).value();
  EXPECT_EQ(destination.Get("alpha").value(), "one");
  EXPECT_EQ(destination.Get("omega").value(), "two");
}

TEST_F(SalvageTest, ExistingDestinationIsNeverReplaced) {
  {
    auto source = tinydb::Database::Open(source_).value();
    ASSERT_TRUE(source.Put("key", "value").Ok());
  }
  {
    auto destination = std::ofstream(destination_, std::ios::binary);
    destination << "owned";
  }
  const auto salvaged = tinydb::salvage::Run(source_, destination_);
  ASSERT_FALSE(salvaged.has_value());
  EXPECT_EQ(salvaged.error().Code(), tinydb::StatusCode::InvalidArgument);
  auto destination = std::ifstream(destination_, std::ios::binary);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>{destination}, {}), "owned");
}

TEST_F(SalvageTest, LiveSourceIsRejectedInsteadOfBeingScannedInconsistently) {
  auto source = tinydb::Database::Open(source_).value();
  ASSERT_TRUE(source.Put("key", "value").Ok());

  const auto salvaged = tinydb::salvage::Run(source_, destination_);
  ASSERT_FALSE(salvaged.has_value());
  EXPECT_EQ(salvaged.error().Code(), tinydb::StatusCode::Busy);
  EXPECT_FALSE(std::filesystem::exists(destination_));
}

}  // namespace
