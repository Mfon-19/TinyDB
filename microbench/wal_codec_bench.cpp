#include <benchmark/benchmark.h>

#include "fixture.h"
#include "storage/page_codec.h"
#include "wal/wal_codec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

class WalFixture final {
 public:
  explicit WalFixture(std::size_t page_count) {
    pages_.reserve(page_count);
    headers_.reserve(page_count);
    auto payload = tinydb::microbench::Bytes(127);
    for (auto index = std::size_t{0}; index < page_count; ++index) {
      payload.front() = static_cast<std::byte>(index & 0xFFU);
      const auto page_id = tinydb::FIRST_DATA_PAGE_ID + static_cast<tinydb::page_id_t>(index);
      const auto page_lsn = COMMIT_LSN;
      const auto owner_value_id = page_id;
      const auto chunk_index = std::uint32_t{0};
      const auto next_page_id = tinydb::HEADER_PAGE_ID;
      pages_.push_back(tinydb::microbench::Take(
          tinydb::storage::EncodeOverflowPage(page_id, page_lsn, owner_value_id, chunk_index, next_page_id, payload)));
      headers_.push_back(tinydb::microbench::Take(
          tinydb::storage::DecodeDataPageHeader(std::as_bytes(std::span{pages_.back()}), page_id)));
    }

    proved_views_.reserve(page_count);
    fallback_views_.reserve(page_count);
    for (auto index = std::size_t{0}; index < page_count; ++index) {
      const auto bytes = std::span<const char, tinydb::PAGE_SIZE>{pages_[index]};
      proved_views_.push_back(tinydb::wal_format::PageImageView{
          .page_id = headers_[index].page_id,
          .bytes = bytes,
          .validated_header = &headers_[index],
      });
      fallback_views_.push_back(tinydb::wal_format::PageImageView{
          .page_id = headers_[index].page_id,
          .bytes = bytes,
      });
    }

    state_ = tinydb::txn::DatabaseState{
        .root_page_id = tinydb::FIRST_DATA_PAGE_ID,
        .allocator_root_page_id = tinydb::HEADER_PAGE_ID,
        .logical_page_count = tinydb::FIRST_DATA_PAGE_ID + static_cast<tinydb::page_id_t>(page_count),
        .visible_lsn = COMMIT_LSN,
        .checkpoint_lsn = 0,
    };
    encoded_ = tinydb::microbench::Take(tinydb::wal_format::EncodeTransaction(COMMIT_LSN, proved_views_, state_));
    const auto fallback =
        tinydb::microbench::Take(tinydb::wal_format::EncodeTransaction(COMMIT_LSN, fallback_views_, state_));
    if (fallback.bytes != encoded_.bytes) {
      throw std::runtime_error("proved and fallback WAL encoders disagree");
    }
    const auto decoded = tinydb::microbench::Take(
        tinydb::wal_format::DecodeTransaction(std::as_bytes(std::span{encoded_.bytes}), COMMIT_LSN));
    if (decoded.commit_lsn != COMMIT_LSN || decoded.pages.size() != page_count ||
        decoded.state.root_page_id != state_.root_page_id ||
        decoded.state.logical_page_count != state_.logical_page_count) {
      throw std::runtime_error("WAL transaction fixture did not round-trip");
    }
  }

  static constexpr auto CommitLsn() -> std::uint64_t { return COMMIT_LSN; }

  auto State() const -> tinydb::txn::DatabaseState { return state_; }
  auto ProvedViews() const -> std::span<const tinydb::wal_format::PageImageView> { return proved_views_; }
  auto FallbackViews() const -> std::span<const tinydb::wal_format::PageImageView> { return fallback_views_; }
  auto EncodedBytes() const -> std::span<const std::byte> { return std::as_bytes(std::span{encoded_.bytes}); }
  auto EncodedSize() const -> std::size_t { return encoded_.bytes.size(); }

 private:
  static constexpr std::uint64_t COMMIT_LSN = 101;

  std::vector<std::array<char, tinydb::PAGE_SIZE>> pages_;
  std::vector<tinydb::storage::DataPageHeader> headers_;
  std::vector<tinydb::wal_format::PageImageView> proved_views_;
  std::vector<tinydb::wal_format::PageImageView> fallback_views_;
  tinydb::txn::DatabaseState state_{};
  tinydb::wal_format::EncodedTransaction encoded_{};
};

void ReportBytes(benchmark::State &state, std::size_t bytes) {
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(bytes));
}

void EncodeTransaction(benchmark::State &state, bool use_proof) {
  const auto fixture = WalFixture(static_cast<std::size_t>(state.range(0)));
  const auto views = use_proof ? fixture.ProvedViews() : fixture.FallbackViews();
  for ([[maybe_unused]] auto iteration : state) {
    auto encoded = tinydb::wal_format::EncodeTransaction(WalFixture::CommitLsn(), views, fixture.State());
    if (!encoded) {
      state.SkipWithError("EncodeTransaction failed");
      return;
    }
    benchmark::DoNotOptimize(encoded->bytes.data());
    benchmark::DoNotOptimize(encoded->bytes.size());
    benchmark::ClobberMemory();
  }
  ReportBytes(state, fixture.EncodedSize());
}

void BmWalCodecEncodeTransactionProved(benchmark::State &state) { EncodeTransaction(state, true); }

void BmWalCodecEncodeTransactionFallback(benchmark::State &state) { EncodeTransaction(state, false); }

void BmWalCodecDecodeTransaction(benchmark::State &state) {
  const auto fixture = WalFixture(static_cast<std::size_t>(state.range(0)));
  for ([[maybe_unused]] auto iteration : state) {
    auto decoded = tinydb::wal_format::DecodeTransaction(fixture.EncodedBytes(), WalFixture::CommitLsn());
    if (!decoded) {
      state.SkipWithError("DecodeTransaction failed");
      return;
    }
    benchmark::DoNotOptimize(decoded->pages.data());
    benchmark::DoNotOptimize(decoded->pages.size());
    benchmark::ClobberMemory();
  }
  ReportBytes(state, fixture.EncodedSize());
}

void WalPageCounts(benchmark::internal::Benchmark *registration) {
  registration->ArgName("pages")->Arg(1)->Arg(4)->Arg(16);
}

BENCHMARK(BmWalCodecEncodeTransactionProved)->Apply(WalPageCounts);
BENCHMARK(BmWalCodecEncodeTransactionFallback)->Apply(WalPageCounts);
BENCHMARK(BmWalCodecDecodeTransaction)->Apply(WalPageCounts);

}  // namespace
