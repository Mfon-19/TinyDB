#include <gtest/gtest.h>

#include "storage/encoding.h"
#include "storage/superblock.h"
#include "util/crc32.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

using tinydb::StatusCode;
using tinydb::storage::DatabaseUuid;
using tinydb::storage::SelectedSuperblock;
using tinydb::storage::Superblock;
using tinydb::storage::SuperblockPage;
using tinydb::storage::SuperblockSlot;

auto Fixture(std::uint64_t generation = 0x0102030405060708ULL) -> Superblock {
  auto uuid = DatabaseUuid{};
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    uuid[i] = static_cast<std::byte>(i);
  }
  return Superblock{
      .database_uuid = uuid,
      .generation = generation,
      .checkpoint_lsn = 0x1112131415161718ULL,
      .transaction_id = 0x2122232425262728ULL,
      .root_page_id = 2,
      .allocator_root_page_id = 3,
      .high_water_page_id = 4,
  };
}

void Reseal(SuperblockPage &page) {
  auto bytes = std::span<std::byte>{page};
  ASSERT_TRUE(tinydb::storage::PutLittleEndian<std::uint32_t>(bytes, tinydb::storage::superblock_offset::CHECKSUM, 0));
  ASSERT_TRUE(
      tinydb::storage::PutLittleEndian(bytes, tinydb::storage::superblock_offset::CHECKSUM, tinydb::Crc32(page)));
}

TEST(EncodingTest, FixedWidthIntegersAreLittleEndianAndBoundsChecked) {
  auto encoded = std::array<std::byte, 16>{};
  auto bytes = std::span<std::byte>{encoded};

  EXPECT_TRUE(tinydb::storage::PutLittleEndian<std::uint16_t>(bytes, 1, 0x1234U));
  EXPECT_TRUE(tinydb::storage::PutLittleEndian<std::uint32_t>(bytes, 3, 0x89ABCDEFU));
  EXPECT_TRUE(tinydb::storage::PutLittleEndian<std::uint64_t>(bytes, 7, 0x0102030405060708ULL));
  EXPECT_EQ(encoded[1], std::byte{0x34});
  EXPECT_EQ(encoded[2], std::byte{0x12});
  EXPECT_EQ(encoded[3], std::byte{0xEF});
  EXPECT_EQ(encoded[6], std::byte{0x89});
  EXPECT_EQ(encoded[7], std::byte{0x08});
  EXPECT_EQ(encoded[14], std::byte{0x01});

  EXPECT_EQ(tinydb::storage::GetLittleEndian<std::uint16_t>(encoded, 1), 0x1234U);
  EXPECT_EQ(tinydb::storage::GetLittleEndian<std::uint32_t>(encoded, 3), 0x89ABCDEFU);
  EXPECT_EQ(tinydb::storage::GetLittleEndian<std::uint64_t>(encoded, 7), 0x0102030405060708ULL);
  EXPECT_FALSE(tinydb::storage::PutLittleEndian<std::uint32_t>(bytes, 13, 1));
  EXPECT_EQ(tinydb::storage::GetLittleEndian<std::uint64_t>(encoded, 9), std::nullopt);
}

TEST(SuperblockCodecTest, RoundTripsEveryLogicalField) {
  auto wanted = Fixture();
  wanted.optional_features = 1ULL << 47U;
  const auto encoded = tinydb::storage::EncodeSuperblock(wanted);
  ASSERT_TRUE(encoded.has_value()) << encoded.error().ToString();
  const auto decoded = tinydb::storage::DecodeSuperblock(*encoded);
  ASSERT_TRUE(decoded.has_value()) << decoded.error().ToString();
  EXPECT_EQ(*decoded, wanted);
}

TEST(SuperblockCodecTest, MatchesTheGoldenLittleEndianFixture) {
  const auto encoded = tinydb::storage::EncodeSuperblock(Fixture());
  ASSERT_TRUE(encoded.has_value());

  constexpr auto golden_prefix = std::array<std::byte, 100>{
      std::byte{0x54}, std::byte{0x49}, std::byte{0x4E}, std::byte{0x59}, std::byte{0x44}, std::byte{0x42},
      std::byte{0x30}, std::byte{0x34}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x10}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
      std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}, std::byte{0x09},
      std::byte{0x0A}, std::byte{0x0B}, std::byte{0x0C}, std::byte{0x0D}, std::byte{0x0E}, std::byte{0x0F},
      std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05}, std::byte{0x04}, std::byte{0x03},
      std::byte{0x02}, std::byte{0x01}, std::byte{0x18}, std::byte{0x17}, std::byte{0x16}, std::byte{0x15},
      std::byte{0x14}, std::byte{0x13}, std::byte{0x12}, std::byte{0x11}, std::byte{0x28}, std::byte{0x27},
      std::byte{0x26}, std::byte{0x25}, std::byte{0x24}, std::byte{0x23}, std::byte{0x22}, std::byte{0x21},
      std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x04}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0xA6}, std::byte{0x08}, std::byte{0x5E}, std::byte{0x1D},
  };

  EXPECT_TRUE(std::ranges::equal(golden_prefix, std::span{*encoded}.first(golden_prefix.size())));
  EXPECT_TRUE(std::ranges::all_of(std::span{*encoded}.subspan(golden_prefix.size()),
                                  [](std::byte byte) { return byte == std::byte{0}; }));
}

TEST(SuperblockCodecTest, CompletePageChecksumDetectsEveryFieldAndReservedBytes) {
  const auto original = tinydb::storage::EncodeSuperblock(Fixture());
  ASSERT_TRUE(original.has_value());
  for (const auto offset :
       {tinydb::storage::superblock_offset::FORMAT_MAJOR, tinydb::storage::superblock_offset::FORMAT_MINOR,
        tinydb::storage::superblock_offset::PAGE_SIZE, tinydb::storage::superblock_offset::REQUIRED_FEATURES,
        tinydb::storage::superblock_offset::OPTIONAL_FEATURES, tinydb::storage::superblock_offset::DATABASE_UUID,
        tinydb::storage::superblock_offset::GENERATION, tinydb::storage::superblock_offset::CHECKPOINT_LSN,
        tinydb::storage::superblock_offset::TRANSACTION_ID, tinydb::storage::superblock_offset::ROOT_PAGE_ID,
        tinydb::storage::superblock_offset::ALLOCATOR_ROOT_PAGE_ID,
        tinydb::storage::superblock_offset::HIGH_WATER_PAGE_ID, tinydb::storage::superblock_offset::CHECKSUM,
        tinydb::storage::superblock_offset::ENCODED_BYTES}) {
    auto corrupted = *original;
    corrupted[offset] ^= std::byte{0x01};
    const auto decoded = tinydb::storage::DecodeSuperblock(corrupted);
    ASSERT_FALSE(decoded.has_value()) << "offset=" << offset;
    EXPECT_EQ(decoded.error().Code(), StatusCode::Corruption) << "offset=" << offset;
  }
}

TEST(SuperblockCodecTest, RejectsUnsupportedVersionRequiredFeaturesAndEndian) {
  const auto original = tinydb::storage::EncodeSuperblock(Fixture());
  ASSERT_TRUE(original.has_value());

  for (const auto patch :
       {tinydb::storage::superblock_offset::FORMAT_MAJOR, tinydb::storage::superblock_offset::FORMAT_MINOR,
        tinydb::storage::superblock_offset::REQUIRED_FEATURES}) {
    auto unsupported = *original;
    if (patch == tinydb::storage::superblock_offset::REQUIRED_FEATURES) {
      ASSERT_TRUE(tinydb::storage::PutLittleEndian<std::uint64_t>(unsupported, patch, 1));
    } else {
      ASSERT_TRUE(tinydb::storage::PutLittleEndian<std::uint16_t>(unsupported, patch, 2));
    }
    Reseal(unsupported);
    const auto decoded = tinydb::storage::DecodeSuperblock(unsupported);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().Code(), StatusCode::UnsupportedFormat);
  }

  auto opposite_endian = *original;
  std::ranges::reverse(std::span{opposite_endian}.subspan(tinydb::storage::superblock_offset::FORMAT_MAJOR, 2));
  std::ranges::reverse(std::span{opposite_endian}.subspan(tinydb::storage::superblock_offset::PAGE_SIZE, 4));
  Reseal(opposite_endian);
  const auto decoded = tinydb::storage::DecodeSuperblock(opposite_endian);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().Code(), StatusCode::UnsupportedFormat);
}

TEST(SuperblockCodecTest, AllowsUnknownOptionalFeatures) {
  auto wanted = Fixture();
  wanted.optional_features = 1ULL << 63U;
  const auto encoded = tinydb::storage::EncodeSuperblock(wanted);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(tinydb::storage::DecodeSuperblock(*encoded), wanted);
}

TEST(SuperblockSelectionTest, ChoosesHighestValidGeneration) {
  const auto page_a = tinydb::storage::EncodeSuperblock(Fixture(7));
  const auto page_b = tinydb::storage::EncodeSuperblock(Fixture(8));
  ASSERT_TRUE(page_a.has_value());
  ASSERT_TRUE(page_b.has_value());

  const auto selected = tinydb::storage::SelectSuperblock(*page_a, *page_b);
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected->slot, SuperblockSlot::B);
  EXPECT_EQ(selected->value.generation, 8U);
}

TEST(SuperblockSelectionTest, OneValidCopySurvivesAnyDamageToTheOther) {
  const auto valid = tinydb::storage::EncodeSuperblock(Fixture(9));
  ASSERT_TRUE(valid.has_value());
  auto damaged = *valid;
  damaged[tinydb::storage::superblock_offset::GENERATION] ^= std::byte{0x80};

  const auto first = tinydb::storage::SelectSuperblock(*valid, damaged);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->slot, SuperblockSlot::A);
  const auto second = tinydb::storage::SelectSuperblock(damaged, *valid);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->slot, SuperblockSlot::B);
}

TEST(SuperblockSelectionTest, EveryTornWriteBoundaryLeavesAValidGeneration) {
  const auto page_a = tinydb::storage::EncodeSuperblock(Fixture(2));
  const auto old_b = tinydb::storage::EncodeSuperblock(Fixture(1));
  const auto new_b = tinydb::storage::EncodeSuperblock(Fixture(3));
  ASSERT_TRUE(page_a.has_value());
  ASSERT_TRUE(old_b.has_value());
  ASSERT_TRUE(new_b.has_value());

  for (std::size_t cut = 0; cut <= tinydb::PAGE_SIZE; ++cut) {
    auto torn_b = *old_b;
    std::ranges::copy_n(new_b->begin(), static_cast<std::ptrdiff_t>(cut), torn_b.begin());
    const auto selected = tinydb::storage::SelectSuperblock(*page_a, torn_b);
    ASSERT_TRUE(selected.has_value()) << "cut=" << cut;
    EXPECT_TRUE(selected->value.generation == 2 || selected->value.generation == 3) << "cut=" << cut;
  }
}

TEST(SuperblockSelectionTest, EqualGenerationsMustDescribeIdenticalState) {
  auto first = Fixture(5);
  auto second = first;
  ++second.transaction_id;
  const auto page_a = tinydb::storage::EncodeSuperblock(first);
  const auto page_b = tinydb::storage::EncodeSuperblock(second);
  ASSERT_TRUE(page_a.has_value());
  ASSERT_TRUE(page_b.has_value());

  const auto selected = tinydb::storage::SelectSuperblock(*page_a, *page_b);
  ASSERT_FALSE(selected.has_value());
  EXPECT_EQ(selected.error().Code(), StatusCode::Corruption);
}

}  // namespace
