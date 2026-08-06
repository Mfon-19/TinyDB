#include "fixture.h"

#include "btree/page_source.h"
#include "btree/page_view.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tinydb::microbench::MakeInternalFixture;
using tinydb::microbench::MakeLeafFixture;
using tinydb::microbench::Take;

auto RecordCount(const benchmark::State &state) -> std::size_t { return static_cast<std::size_t>(state.range(0)); }

auto QueryPlan(const std::vector<std::string> &keys, std::size_t count) -> std::vector<std::string_view> {
  auto queries = std::vector<std::string_view>{};
  queries.reserve(count);
  auto index = std::size_t{0};
  for (auto query = std::size_t{0}; query < count; ++query) {
    queries.emplace_back(keys[index]);
    index = (index + 37U) % count;
  }
  return queries;
}

template <typename Operation>
void RunQueries(benchmark::State &state, const std::vector<std::string_view> &queries, Operation operation) {
  auto index = std::size_t{0};
  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(operation(queries[index]));
    if (++index == queries.size()) {
      index = 0;
    }
  }
  state.SetItemsProcessed(state.iterations());
}

void BmPageViewLeafOpenRaw(benchmark::State &state) {
  const auto fixture = MakeLeafFixture(RecordCount(state));
  for ([[maybe_unused]] auto iteration : state) {
    auto view = tinydb::LeafPageView::Open(fixture.page.data(), fixture.header.page_id);
    benchmark::DoNotOptimize(view);
  }
  state.SetItemsProcessed(state.iterations());
}

void BmPageViewLeafOpenProved(benchmark::State &state) {
  const auto fixture = MakeLeafFixture(RecordCount(state));
  const auto page = tinydb::PageHandle(fixture.header.page_id, fixture.page.data(), std::shared_ptr<const void>{},
                                       &fixture.header, true);
  for ([[maybe_unused]] auto iteration : state) {
    auto view = tinydb::LeafPageView::Open(page);
    benchmark::DoNotOptimize(view);
  }
  state.SetItemsProcessed(state.iterations());
}

void BmPageViewInternalOpenRaw(benchmark::State &state) {
  const auto fixture = MakeInternalFixture(RecordCount(state));
  for ([[maybe_unused]] auto iteration : state) {
    auto view = tinydb::InternalPageView::Open(fixture.page.data(), fixture.header.page_id);
    benchmark::DoNotOptimize(view);
  }
  state.SetItemsProcessed(state.iterations());
}

void BmPageViewInternalOpenProved(benchmark::State &state) {
  const auto fixture = MakeInternalFixture(RecordCount(state));
  const auto page = tinydb::PageHandle(fixture.header.page_id, fixture.page.data(), std::shared_ptr<const void>{},
                                       &fixture.header, true);
  for ([[maybe_unused]] auto iteration : state) {
    auto view = tinydb::InternalPageView::Open(page);
    benchmark::DoNotOptimize(view);
  }
  state.SetItemsProcessed(state.iterations());
}

void RunLeafLowerBound(benchmark::State &state, bool hit) {
  const auto fixture = MakeLeafFixture(RecordCount(state));
  const auto view = Take(tinydb::LeafPageView::Open(fixture.page.data(), fixture.header.page_id));
  const auto queries = QueryPlan(hit ? fixture.keys : fixture.missing_keys, fixture.keys.size());
  RunQueries(state, queries, [&view](std::string_view key) { return view.LowerBound(key); });
}

void BmPageViewLeafLowerBoundHit(benchmark::State &state) { RunLeafLowerBound(state, true); }

void BmPageViewLeafLowerBoundMiss(benchmark::State &state) { RunLeafLowerBound(state, false); }

void RunLeafGet(benchmark::State &state, bool hit) {
  const auto fixture = MakeLeafFixture(RecordCount(state));
  const auto view = Take(tinydb::LeafPageView::Open(fixture.page.data(), fixture.header.page_id));
  const auto queries = QueryPlan(hit ? fixture.keys : fixture.missing_keys, fixture.keys.size());
  RunQueries(state, queries, [&view](std::string_view key) { return view.Get(key); });
}

void BmPageViewLeafGetHit(benchmark::State &state) { RunLeafGet(state, true); }

void BmPageViewLeafGetMiss(benchmark::State &state) { RunLeafGet(state, false); }

void RunInternalFindChild(benchmark::State &state, bool hit) {
  const auto fixture = MakeInternalFixture(RecordCount(state));
  const auto view = Take(tinydb::InternalPageView::Open(fixture.page.data(), fixture.header.page_id));
  const auto &keys = hit ? fixture.keys : fixture.missing_keys;
  const auto queries = QueryPlan(keys, hit ? keys.size() : keys.size() - 1U);
  RunQueries(state, queries, [&view](std::string_view key) { return view.FindChildIndex(key); });
}

void BmPageViewInternalFindChildHit(benchmark::State &state) { RunInternalFindChild(state, true); }

void BmPageViewInternalFindChildBetween(benchmark::State &state) { RunInternalFindChild(state, false); }

#define TINYDB_PAGE_COUNTS(BENCHMARK_NAME) BENCHMARK(BENCHMARK_NAME)->Arg(16)->Arg(64)->Arg(128)->ArgName("records")

TINYDB_PAGE_COUNTS(BmPageViewLeafOpenRaw);
TINYDB_PAGE_COUNTS(BmPageViewLeafOpenProved);
TINYDB_PAGE_COUNTS(BmPageViewInternalOpenRaw);
TINYDB_PAGE_COUNTS(BmPageViewInternalOpenProved);
TINYDB_PAGE_COUNTS(BmPageViewLeafLowerBoundHit);
TINYDB_PAGE_COUNTS(BmPageViewLeafLowerBoundMiss);
TINYDB_PAGE_COUNTS(BmPageViewLeafGetHit);
TINYDB_PAGE_COUNTS(BmPageViewLeafGetMiss);
TINYDB_PAGE_COUNTS(BmPageViewInternalFindChildHit);
TINYDB_PAGE_COUNTS(BmPageViewInternalFindChildBetween);

#undef TINYDB_PAGE_COUNTS

}  // namespace
