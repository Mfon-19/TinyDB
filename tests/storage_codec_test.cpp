#include <gtest/gtest.h>

#include "btree/internal_page_builder.h"
#include "btree/leaf_page_builder.h"
#include "btree/page_format.h"
#include "btree/page_view.h"
#include "storage/encoding.h"
#include "storage/page_codec.h"
#include "storage/superblock.h"
#include "util/crc32.h"
#include "wal/wal_codec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

/*
** Persistent-format tests use byte-distinct golden values and mutate encoded
** fields only after deliberately resealing their checksums. This separates
** framing/checksum failures from semantic validation and detects accidental
** dependence on host endianness, padding, or native struct layout.
*/
namespace {

using tinydb::DatabaseUuid;
using tinydb::StatusCode;
using tinydb::storage::SelectedSuperblock;
using tinydb::storage::Superblock;
using tinydb::storage::SuperblockPage;
using tinydb::storage::SuperblockSlot;

// Deliberately choose asymmetric, byte-distinct numbers. A native-endian or
// misaligned field will then disagree visibly with the golden fixture instead
// of accidentally passing because the value is zero or palindromic.
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
  // Tests that change semantic fields must recompute CRC so they exercise
  // version/feature validation rather than stopping at checksum validation.
  auto bytes = std::span<std::byte>{page};
  ASSERT_TRUE(tinydb::storage::PutLittleEndian<std::uint32_t>(bytes, tinydb::storage::superblock_offset::CHECKSUM, 0));
  ASSERT_TRUE(
      tinydb::storage::PutLittleEndian(bytes, tinydb::storage::superblock_offset::CHECKSUM, tinydb::Crc32(page)));
}

void ResealWalHeader(std::vector<char> &header) {
  auto bytes = std::as_writable_bytes(std::span{header});
  ASSERT_TRUE(tinydb::storage::PutLittleEndian<std::uint32_t>(bytes, tinydb::wal_format::header_offset::CHECKSUM, 0));
  ASSERT_TRUE(
      tinydb::storage::PutLittleEndian(bytes, tinydb::wal_format::header_offset::CHECKSUM, tinydb::Crc32(bytes)));
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

  // This byte array is intentionally independent of EncodeSuperblock helpers.
  // It is the compatibility contract: refactoring the codec may not change it
  // merely because round-trip tests still agree with themselves.
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
  // Touch one byte in every logical field plus the first reserved byte. The
  // decoder must not leave any persistent region outside checksum coverage.
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

  // Resealing proves the classification is UnsupportedFormat because of
  // semantics, not Corruption because bytes were damaged in transit.
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

  // Model a sector/device tear at every possible byte: a prefix of new B lands
  // over old B while A remains the previous durable generation.
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

TEST(DataPageCodecTest, FreeExtentPageRoundTripsSortedRetirementMetadata) {
  const auto extents = std::array{
      tinydb::storage::FreeExtent{.first_page_id = 4, .page_count = 3, .retire_lsn = 11},
      tinydb::storage::FreeExtent{.first_page_id = 20, .page_count = 5, .retire_lsn = 17},
  };
  const auto encoded = tinydb::storage::EncodeFreeExtentPage(2, 0x0102030405060708ULL, 3, extents);
  ASSERT_TRUE(encoded.has_value());
  const auto bytes = std::as_bytes(std::span{*encoded});
  const auto decoded = tinydb::storage::DecodeFreeExtentPage(bytes, 2);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->next_page_id, 3U);
  EXPECT_EQ(decoded->extents, std::vector(extents.begin(), extents.end()));
  EXPECT_EQ(tinydb::storage::DecodeDataPageHeader(bytes, 2)->page_lsn, 0x0102030405060708ULL);

  auto adjacent = extents;
  adjacent[1].first_page_id = 7;
  EXPECT_EQ(tinydb::storage::EncodeFreeExtentPage(2, 0, 0, adjacent).error().Code(), StatusCode::InvalidArgument);
}

TEST(DataPageCodecTest, OverflowLengthsIdentityAndChecksumAreValidated) {
  // Include NUL and high-bit bytes to prove values are opaque bytes rather
  // than C strings or signed characters.
  const auto payload = std::array{std::byte{0x00}, std::byte{0x7F}, std::byte{0x80}, std::byte{0xFF}};
  const auto encoded = tinydb::storage::EncodeOverflowPage(7, 99, 12, 8, payload);
  ASSERT_TRUE(encoded.has_value());
  const auto bytes = std::as_bytes(std::span{*encoded});
  const auto decoded = tinydb::storage::DecodeOverflowPage(bytes, 7);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->total_value_bytes, 12U);
  EXPECT_EQ(decoded->next_page_id, 8U);
  EXPECT_EQ(decoded->payload, std::vector<std::byte>(payload.begin(), payload.end()));

  EXPECT_EQ(tinydb::storage::DecodeOverflowPage(bytes, 6).error().Code(), StatusCode::Corruption);
  auto corrupted = *encoded;
  corrupted.back() ^= 0x01;
  EXPECT_EQ(tinydb::storage::DecodeOverflowPage(std::as_bytes(std::span{corrupted}), 7).error().Code(),
            StatusCode::Corruption);
}

TEST(DataPageCodecTest, RejectsUnsupportedVersionAndOppositeEndianIdentity) {
  const auto encoded = tinydb::storage::EncodeFreeExtentPage(2, 0, 0, std::span<const tinydb::storage::FreeExtent>{});
  ASSERT_TRUE(encoded.has_value());

  auto unsupported = *encoded;
  auto unsupported_bytes = std::as_writable_bytes(std::span{unsupported});
  ASSERT_TRUE(tinydb::storage::PutLittleEndian(unsupported_bytes, tinydb::storage::data_page_offset::FORMAT_VERSION,
                                               std::uint16_t{2}));
  ASSERT_TRUE(tinydb::storage::FinalizeDataPage(unsupported_bytes).Ok());
  EXPECT_EQ(tinydb::storage::DecodeDataPageHeader(std::as_bytes(std::span{unsupported}), 2).error().Code(),
            StatusCode::UnsupportedFormat);

  auto opposite_endian = *encoded;
  auto page_id =
      std::span{opposite_endian}.subspan(tinydb::storage::data_page_offset::PAGE_ID, sizeof(tinydb::page_id_t));
  std::ranges::reverse(page_id);
  auto opposite_bytes = std::as_writable_bytes(std::span{opposite_endian});
  ASSERT_TRUE(tinydb::storage::FinalizeDataPage(opposite_bytes).Ok());
  EXPECT_EQ(tinydb::storage::DecodeDataPageHeader(std::as_bytes(std::span{opposite_endian}), 2).error().Code(),
            StatusCode::Corruption);
}

TEST(DataPageCodecTest, LeafAndInternalPagesUseTheValidatedCommonHeader) {
  // Exercise the one builder/view encoding path, ensuring tree pages did not
  // retain a parallel legacy decoder beside the common page codec.
  auto leaf_page = std::array<char, tinydb::PAGE_SIZE>{};
  auto leaf = tinydb::LeafPageBuilder{};
  leaf.Upsert("alpha", "one");
  leaf.Upsert("omega", "two");
  leaf.Store(leaf_page.data(), 2);
  EXPECT_TRUE(tinydb::ValidateTreePage(leaf_page.data(), 2).Ok());
  const auto loaded_leaf = tinydb::LeafPageView::Open(leaf_page.data(), 2).value();
  EXPECT_EQ(loaded_leaf.Get("alpha"), std::optional<std::string_view>{"one"});
  EXPECT_EQ(loaded_leaf.Get("omega"), std::optional<std::string_view>{"two"});

  auto internal_page = std::array<char, tinydb::PAGE_SIZE>{};
  const auto internal = tinydb::InternalPageBuilder{2, "middle", 3};
  internal.Store(internal_page.data(), 4);
  EXPECT_TRUE(tinydb::ValidateTreePage(internal_page.data(), 4).Ok());
  const auto loaded_internal = tinydb::InternalPageView::Open(internal_page.data(), 4).value();
  EXPECT_EQ(loaded_internal.ChildAt(0), 2U);
  EXPECT_EQ(loaded_internal.ChildAt(1), 3U);
}

TEST(WalCodecTest, HeaderMatchesGoldenBytesAndRoundTrips) {
  auto uuid = tinydb::DatabaseUuid{};
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    uuid[i] = static_cast<std::byte>(i);
  }
  const auto wanted = tinydb::wal_format::Header{
      .database_uuid = uuid,
      .segment_id = 0x0102030405060708ULL,
  };
  const auto encoded = tinydb::wal_format::EncodeHeader(wanted);
  ASSERT_TRUE(encoded.has_value());
  constexpr auto golden = std::array<std::byte, 80>{
      std::byte{0x54}, std::byte{0x49}, std::byte{0x4E}, std::byte{0x59}, std::byte{0x57}, std::byte{0x4C},
      std::byte{0x30}, std::byte{0x34}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x50}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
      std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}, std::byte{0x09},
      std::byte{0x0A}, std::byte{0x0B}, std::byte{0x0C}, std::byte{0x0D}, std::byte{0x0E}, std::byte{0x0F},
      std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05}, std::byte{0x04}, std::byte{0x03},
      std::byte{0x02}, std::byte{0x01}, std::byte{0x50}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x78}, std::byte{0xFD},
      std::byte{0x15}, std::byte{0x77}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00},
  };
  const auto bytes = std::as_bytes(std::span{*encoded});
  EXPECT_TRUE(std::ranges::equal(golden, bytes));
  EXPECT_EQ(tinydb::wal_format::DecodeHeader(bytes), wanted);
}

TEST(WalCodecTest, RecordMatchesGoldenBytesAndDetectsCorruption) {
  constexpr auto payload = std::array{std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};
  const auto encoded =
      tinydb::wal_format::EncodeRecord(tinydb::wal_format::RecordType::PageImage, 0x0102030405060708ULL, 80, payload);
  ASSERT_TRUE(encoded.has_value());
  constexpr auto golden = std::array<std::byte, 35>{
      std::byte{0x23}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05},
      std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01}, std::byte{0x50}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x8F}, std::byte{0xF3}, std::byte{0x40}, std::byte{0x2D}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC},
  };
  const auto bytes = std::as_bytes(std::span{*encoded});
  EXPECT_TRUE(std::ranges::equal(golden, bytes));
  const auto decoded = tinydb::wal_format::DecodeRecord(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->type, tinydb::wal_format::RecordType::PageImage);
  EXPECT_EQ(decoded->transaction_id, 0x0102030405060708ULL);
  EXPECT_EQ(decoded->lsn, 80U);
  EXPECT_EQ(decoded->payload, std::vector<std::byte>(payload.begin(), payload.end()));

  for (const auto offset : {std::size_t{0}, std::size_t{4}, std::size_t{8}, std::size_t{16}, std::size_t{24},
                            std::size_t{28}, std::size_t{34}}) {
    auto corrupted = *encoded;
    corrupted[offset] ^= 0x01;
    EXPECT_EQ(tinydb::wal_format::DecodeRecord(std::as_bytes(std::span{corrupted})).error().Code(),
              StatusCode::Corruption)
        << "offset=" << offset;
  }
}

TEST(WalCodecTest, RejectsUnsupportedVersionsFeaturesAndOppositeEndianHeader) {
  auto uuid = tinydb::DatabaseUuid{};
  uuid.back() = std::byte{1};
  const auto original = tinydb::wal_format::EncodeHeader(tinydb::wal_format::Header{.database_uuid = uuid});
  ASSERT_TRUE(original.has_value());

  for (const auto offset :
       {tinydb::wal_format::header_offset::FORMAT_MAJOR, tinydb::wal_format::header_offset::FORMAT_MINOR}) {
    auto unsupported = *original;
    ASSERT_TRUE(
        tinydb::storage::PutLittleEndian(std::as_writable_bytes(std::span{unsupported}), offset, std::uint16_t{2}));
    ResealWalHeader(unsupported);
    EXPECT_EQ(tinydb::wal_format::DecodeHeader(std::as_bytes(std::span{unsupported})).error().Code(),
              StatusCode::UnsupportedFormat);
  }

  auto required = *original;
  ASSERT_TRUE(tinydb::storage::PutLittleEndian(std::as_writable_bytes(std::span{required}),
                                               tinydb::wal_format::header_offset::REQUIRED_FEATURES, std::uint64_t{1}));
  ResealWalHeader(required);
  EXPECT_EQ(tinydb::wal_format::DecodeHeader(std::as_bytes(std::span{required})).error().Code(),
            StatusCode::UnsupportedFormat);

  auto opposite_endian = *original;
  std::ranges::reverse(
      std::span{opposite_endian}.subspan(tinydb::wal_format::header_offset::HEADER_BYTES, sizeof(std::uint32_t)));
  ResealWalHeader(opposite_endian);
  EXPECT_EQ(tinydb::wal_format::DecodeHeader(std::as_bytes(std::span{opposite_endian})).error().Code(),
            StatusCode::UnsupportedFormat);
}

}  // namespace
