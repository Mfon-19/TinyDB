#include <benchmark/benchmark.h>

#include "btree/page_source.h"
#include "btree/value_storage.h"
#include "fixture.h"
#include "storage/page_codec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

class DenseOverflowReader final : public tinydb::PageReader {
 public:
  DenseOverflowReader(std::size_t page_count, bool attach_header_proof) : attach_header_proof_(attach_header_proof) {
    auto backing = std::make_shared<Backing>();
    backing->pages.reserve(page_count);
    auto payload = tinydb::microbench::Bytes(tinydb::storage::OVERFLOW_PAGE_PAYLOAD_BYTES);
    for (auto index = std::size_t{0}; index < page_count; ++index) {
      payload.front() = static_cast<std::byte>(index & 0xFFU);
      const auto page_id = tinydb::FIRST_DATA_PAGE_ID + static_cast<tinydb::page_id_t>(index);
      const auto next_page_id = index + 1U == page_count ? tinydb::HEADER_PAGE_ID : page_id + 1U;
      auto encoded = tinydb::microbench::Take(tinydb::storage::EncodeOverflowPage(
          page_id, PAGE_LSN, tinydb::FIRST_DATA_PAGE_ID, static_cast<std::uint32_t>(index), next_page_id, payload));
      auto header =
          tinydb::microbench::Take(tinydb::storage::DecodeDataPageHeader(std::as_bytes(std::span{encoded}), page_id));
      backing->pages.push_back(StoredPage{.bytes = encoded, .header = header});
    }
    backing_ = std::move(backing);
  }

  auto Read(tinydb::page_id_t page_id) -> tinydb::Result<tinydb::PageHandle> override {
    if (page_id < tinydb::FIRST_DATA_PAGE_ID) {
      return std::unexpected(tinydb::Status::Corruption("microbenchmark overflow page ID is invalid"));
    }
    const auto index = static_cast<std::size_t>(page_id - tinydb::FIRST_DATA_PAGE_ID);
    if (index >= backing_->pages.size()) {
      return std::unexpected(tinydb::Status::Corruption("microbenchmark overflow page is absent"));
    }
    const auto &page = backing_->pages[index];
    auto keepalive = std::shared_ptr<const void>{backing_};
    return tinydb::PageHandle(page_id, page.bytes.data(), std::move(keepalive),
                              attach_header_proof_ ? &page.header : nullptr);
  }

  auto Descriptor() const -> tinydb::OverflowValueDescriptor {
    return {
        .total_value_bytes = static_cast<std::uint64_t>(TotalBytes()),
        .first_page_id = tinydb::FIRST_DATA_PAGE_ID,
    };
  }

  auto RawCopyControl() const -> std::string {
    auto output = std::string{};
    output.reserve(TotalBytes());
    for (const auto &page : backing_->pages) {
      output.append(page.bytes.data() + PAYLOAD_OFFSET, tinydb::storage::OVERFLOW_PAGE_PAYLOAD_BYTES);
    }
    return output;
  }

  auto TotalBytes() const -> std::size_t {
    return backing_->pages.size() * tinydb::storage::OVERFLOW_PAGE_PAYLOAD_BYTES;
  }

 private:
  static constexpr std::uint64_t PAGE_LSN = 17;
  static constexpr std::size_t PAYLOAD_OFFSET = tinydb::PAGE_SIZE - tinydb::storage::OVERFLOW_PAGE_PAYLOAD_BYTES;

  struct StoredPage {
    std::array<char, tinydb::PAGE_SIZE> bytes;
    tinydb::storage::DataPageHeader header;
  };

  struct Backing {
    std::vector<StoredPage> pages;
  };

  std::shared_ptr<const Backing> backing_;
  bool attach_header_proof_;
};

void ReportBytes(benchmark::State &state, std::size_t bytes) {
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(bytes));
}

void CopyOverflowValue(benchmark::State &state, bool attach_header_proof) {
  auto pages = DenseOverflowReader(static_cast<std::size_t>(state.range(0)), attach_header_proof);
  const auto descriptor = pages.Descriptor();
  const auto expected = pages.RawCopyControl();
  if (tinydb::microbench::Take(tinydb::CopyOverflowValue(&pages, descriptor)) != expected) {
    state.SkipWithError("overflow fixture produced different bytes");
    return;
  }

  for ([[maybe_unused]] auto iteration : state) {
    auto output = tinydb::CopyOverflowValue(&pages, descriptor);
    if (!output) {
      state.SkipWithError("CopyOverflowValue failed");
      return;
    }
    benchmark::DoNotOptimize(output->data());
    benchmark::DoNotOptimize(output->size());
    benchmark::ClobberMemory();
  }
  ReportBytes(state, pages.TotalBytes());
}

void BmOverflowCopyValueProved(benchmark::State &state) { CopyOverflowValue(state, true); }

void BmOverflowCopyValueUnproved(benchmark::State &state) { CopyOverflowValue(state, false); }

void BmOverflowValueRawCopyControl(benchmark::State &state) {
  const auto pages = DenseOverflowReader(static_cast<std::size_t>(state.range(0)), true);
  const auto expected_bytes = pages.TotalBytes();
  if (pages.RawCopyControl().size() != expected_bytes) {
    state.SkipWithError("raw overflow fixture has the wrong size");
    return;
  }

  for ([[maybe_unused]] auto iteration : state) {
    auto output = pages.RawCopyControl();
    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(output.size());
    benchmark::ClobberMemory();
  }
  ReportBytes(state, expected_bytes);
}

void OverflowPageCounts(benchmark::internal::Benchmark *registration) {
  registration->ArgName("pages")->Arg(1)->Arg(4)->Arg(16)->Arg(64);
}

BENCHMARK(BmOverflowCopyValueProved)->Apply(OverflowPageCounts);
BENCHMARK(BmOverflowCopyValueUnproved)->Apply(OverflowPageCounts);
BENCHMARK(BmOverflowValueRawCopyControl)->Apply(OverflowPageCounts);

}  // namespace
