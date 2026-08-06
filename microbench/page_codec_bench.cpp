#include "fixture.h"

#include "storage/page.h"
#include "storage/page_codec.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace tinydb::microbench {
namespace {

void SetPageRates(benchmark::State &state) {
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(PAGE_SIZE));
}

void BmPageCodecFinalizeDataPage(benchmark::State &state) {
  auto fixture = MakeLeafFixture(64);
  auto page = std::as_writable_bytes(std::span{fixture.page});

  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(page);
    storage::FinalizeDataPage(page);
    benchmark::ClobberMemory();
  }

  SetPageRates(state);
}

BENCHMARK(BmPageCodecFinalizeDataPage);

void BmPageCodecDecodeDataPageHeader(benchmark::State &state) {
  auto fixture = MakeLeafFixture(64);
  auto page = std::as_bytes(std::span{fixture.page});
  auto page_id = fixture.header.page_id;

  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(page);
    benchmark::DoNotOptimize(page_id);
    auto header = storage::DecodeDataPageHeader(page, page_id);
    benchmark::DoNotOptimize(header);
  }

  SetPageRates(state);
}

BENCHMARK(BmPageCodecDecodeDataPageHeader);

void BmPageCodecRewriteDataPageLsn(benchmark::State &state) {
  auto fixture = MakeLeafFixture(64);
  auto page = std::as_writable_bytes(std::span{fixture.page});
  auto page_id = fixture.header.page_id;
  auto page_lsn = fixture.header.page_lsn + 1U;

  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(page);
    benchmark::DoNotOptimize(page_id);
    benchmark::DoNotOptimize(page_lsn);
    auto header = storage::RewriteDataPageLsn(page, page_id, page_lsn);
    benchmark::DoNotOptimize(header);
    benchmark::ClobberMemory();
    ++page_lsn;
  }

  SetPageRates(state);
}

BENCHMARK(BmPageCodecRewriteDataPageLsn);

struct OverflowFixture {
  std::array<char, PAGE_SIZE> page;
  storage::DataPageHeader header;
};

auto MakeOverflowFixture() -> OverflowFixture {
  auto payload = Bytes(storage::OVERFLOW_PAGE_PAYLOAD_BYTES);
  auto page = Take(storage::EncodeOverflowPage(FIRST_DATA_PAGE_ID, 1, FIRST_DATA_PAGE_ID, 0, HEADER_PAGE_ID, payload));
  auto header = Take(storage::DecodeDataPageHeader(std::as_bytes(std::span{page}), FIRST_DATA_PAGE_ID));
  return {.page = page, .header = header};
}

void BmPageCodecDecodeOverflowPageUnproved(benchmark::State &state) {
  auto fixture = MakeOverflowFixture();
  auto page = std::as_bytes(std::span{fixture.page});
  auto page_id = fixture.header.page_id;

  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(page);
    benchmark::DoNotOptimize(page_id);
    auto decoded = storage::DecodeOverflowPage(page, page_id);
    benchmark::DoNotOptimize(decoded);
  }

  SetPageRates(state);
}

BENCHMARK(BmPageCodecDecodeOverflowPageUnproved);

void BmPageCodecDecodeOverflowPageProved(benchmark::State &state) {
  auto fixture = MakeOverflowFixture();
  auto page = std::as_bytes(std::span{fixture.page});
  auto page_id = fixture.header.page_id;

  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(page);
    benchmark::DoNotOptimize(page_id);
    benchmark::DoNotOptimize(fixture.header);
    auto decoded = storage::DecodeOverflowPage(page, page_id, fixture.header);
    benchmark::DoNotOptimize(decoded);
  }

  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BmPageCodecDecodeOverflowPageProved);

}  // namespace
}  // namespace tinydb::microbench
