#include "fixture.h"

#include "storage/page.h"
#include "storage/page_codec.h"
#include "util/crc32.h"
#include "wal/wal_codec.h"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tinydb::microbench {
namespace {

void SetRates(benchmark::State &state, std::int64_t bytes_per_iteration) {
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() * bytes_per_iteration);
}

void BmCrc32(benchmark::State &state) {
  auto input_bytes = Bytes(static_cast<std::size_t>(state.range(0)));
  auto input = std::span<const std::byte>{input_bytes};

  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(input);
    auto checksum = tinydb::Crc32(input);
    benchmark::DoNotOptimize(checksum);
  }

  SetRates(state, state.range(0));
}

BENCHMARK(BmCrc32)->Arg(64)->Arg(4096)->Arg(4120)->Arg(65536)->ArgName("bytes");

void BmCrc32ZeroedU32(benchmark::State &state) {
  auto input_bytes = Bytes(static_cast<std::size_t>(state.range(0)));
  auto input = std::span<const std::byte>{input_bytes};
  auto zero_offset = static_cast<std::size_t>(state.range(1));

  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(input);
    benchmark::DoNotOptimize(zero_offset);
    auto checksum = tinydb::Crc32WithZeroedU32(input, zero_offset);
    benchmark::DoNotOptimize(checksum);
  }

  SetRates(state, state.range(0));
}

BENCHMARK(BmCrc32ZeroedU32)
    ->Args({static_cast<std::int64_t>(wal_format::HEADER_BYTES),
            static_cast<std::int64_t>(wal_format::header_offset::CHECKSUM)})
    ->Args({static_cast<std::int64_t>(PAGE_SIZE), static_cast<std::int64_t>(storage::data_page_offset::CHECKSUM)})
    ->Args({static_cast<std::int64_t>(wal_format::RECORD_HEADER_BYTES + PAGE_SIZE),
            static_cast<std::int64_t>(wal_format::record_offset::CHECKSUM)})
    ->ArgNames({"bytes", "zero_offset"});

void BmCrc32Combine(benchmark::State &state) {
  auto prefix = Bytes(wal_format::RECORD_HEADER_BYTES);
  auto suffix = Bytes(PAGE_SIZE);
  auto prefix_crc = tinydb::Crc32(prefix);
  auto suffix_crc = tinydb::Crc32(suffix);
  auto suffix_bytes = suffix.size();

  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(prefix_crc);
    benchmark::DoNotOptimize(suffix_crc);
    benchmark::DoNotOptimize(suffix_bytes);
    auto checksum = tinydb::Crc32Combine(prefix_crc, suffix_crc, suffix_bytes);
    benchmark::DoNotOptimize(checksum);
  }

  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BmCrc32Combine);

void BmCrc32Replace(benchmark::State &state) {
  auto fixture = MakeLeafFixture(64);
  auto page = std::as_bytes(std::span{fixture.page});
  constexpr auto zero_checksum = std::array<std::byte, sizeof(std::uint32_t)>{};
  auto replacement = page.subspan(storage::data_page_offset::CHECKSUM, sizeof(std::uint32_t));
  auto original_crc = tinydb::Crc32WithZeroedU32(page, storage::data_page_offset::CHECKSUM);
  auto trailing_bytes = PAGE_SIZE - storage::data_page_offset::CHECKSUM - sizeof(std::uint32_t);

  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(original_crc);
    benchmark::DoNotOptimize(replacement);
    benchmark::DoNotOptimize(trailing_bytes);
    auto checksum = tinydb::Crc32Replace(original_crc, zero_checksum, replacement, trailing_bytes);
    benchmark::DoNotOptimize(checksum);
  }

  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BmCrc32Replace);

}  // namespace
}  // namespace tinydb::microbench
