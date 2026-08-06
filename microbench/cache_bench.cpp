#include "fixture.h"

#include "cache/committed_page_cache.h"
#include "storage/disk_manager.h"
#include "storage/page_codec.h"

#include <benchmark/benchmark.h>

#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using tinydb::microbench::Take;

constexpr auto CACHE_PAGES = std::size_t{256};
constexpr auto CACHE_BYTES = std::size_t{16} << 20U;

class TemporaryDatabase final {
 public:
  TemporaryDatabase()
      : path_(std::filesystem::temp_directory_path() /
              ("tinydb-microbench-cache-" + std::to_string(::getpid()) + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {}

  TemporaryDatabase(const TemporaryDatabase &) = delete;
  auto operator=(const TemporaryDatabase &) -> TemporaryDatabase & = delete;

  ~TemporaryDatabase() {
    auto ignored = std::error_code{};
    std::filesystem::remove(path_, ignored);
  }

  auto Path() const -> const std::filesystem::path & { return path_; }

 private:
  std::filesystem::path path_;
};

class ResidentCache final {
 public:
  ResidentCache() : disk_(Take(tinydb::DiskManager::Open(database_.Path()))), cache_(disk_, CACHE_BYTES, 1) {
    auto images = std::vector<tinydb::cache::CommittedPageImage>{};
    images.reserve(CACHE_PAGES);
    for (auto index = std::size_t{0}; index < CACHE_PAGES; ++index) {
      const auto page_id = tinydb::FIRST_DATA_PAGE_ID + static_cast<tinydb::page_id_t>(index);
      auto encoded = Take(tinydb::storage::EncodeFreeExtentPage(page_id, 1, tinydb::HEADER_PAGE_ID, {}));
      const auto header = Take(tinydb::storage::DecodeDataPageHeader(std::as_bytes(std::span{encoded}), page_id));
      images.push_back(tinydb::cache::CommittedPageImage{
          .header = header,
          .bytes = std::make_unique<tinydb::cache::PageBytes>(encoded),
      });
    }
    const auto logical_page_count = tinydb::FIRST_DATA_PAGE_ID + static_cast<tinydb::page_id_t>(CACHE_PAGES);
    auto plan = Take(cache_.PreparePublication(std::move(images), {}, logical_page_count));
    cache_.Publish(std::move(plan));
    static_cast<void>(Take(cache_.Read(tinydb::FIRST_DATA_PAGE_ID)));
  }

  auto Get() -> tinydb::cache::CommittedPageCache & { return cache_; }

 private:
  TemporaryDatabase database_;
  tinydb::DiskManager disk_;
  tinydb::cache::CommittedPageCache cache_;
};

auto ResidentCacheFixture() -> ResidentCache & {
  static auto fixture = ResidentCache{};
  return fixture;
}

void BmCacheResidentMruHit(benchmark::State &state) {
  auto &cache = ResidentCacheFixture().Get();
  static_cast<void>(Take(cache.Read(tinydb::FIRST_DATA_PAGE_ID)));
  for ([[maybe_unused]] auto iteration : state) {
    auto page = cache.Read(tinydb::FIRST_DATA_PAGE_ID);
    benchmark::DoNotOptimize(page);
  }
  state.SetItemsProcessed(state.iterations());
}

void BmCacheResidentRoundRobin256Pages(benchmark::State &state) {
  auto &cache = ResidentCacheFixture().Get();
  auto index = static_cast<std::size_t>(state.thread_index()) * 37U % CACHE_PAGES;
  for ([[maybe_unused]] auto iteration : state) {
    const auto page_id = tinydb::FIRST_DATA_PAGE_ID + static_cast<tinydb::page_id_t>(index);
    auto page = cache.Read(page_id);
    benchmark::DoNotOptimize(page);
    if (++index == CACHE_PAGES) {
      index = 0;
    }
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BmCacheResidentMruHit)->Threads(1)->Threads(8)->UseRealTime();
BENCHMARK(BmCacheResidentRoundRobin256Pages)->Threads(1)->Threads(8)->UseRealTime();

}  // namespace
