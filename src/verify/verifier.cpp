#include "verify/verifier.h"

#include "btree/page_format.h"
#include "btree/page_view.h"
#include "btree/value_storage.h"
#include "storage/page_codec.h"
#include "txn/contract.h"
#include "txn/database_state.h"
#include "txn/transaction_pages.h"

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace tinydb::verify {
namespace {

auto IsPersistentFailure(const Status &status) -> bool {
  return status.Code() == StatusCode::Corruption || status.Code() == StatusCode::UnsupportedFormat;
}

void AddIssue(VerifyReport *report, const VerifyOptions &options, VerifyIssueKind kind, page_id_t page_id,
              std::string_view message) {
  if (report->issues.size() < options.max_issues) {
    report->issues.push_back(VerifyIssue{.kind = kind, .page_id = page_id, .message = std::string{message}});
  } else {
    report->complete = false;
  }
}

auto Stop(VerifyReport *report, const VerifyOptions &options, VerifyIssueKind kind, page_id_t page_id,
          Status status) -> Status {
  if (!IsPersistentFailure(status)) {
    return status;
  }
  AddIssue(report, options, kind, page_id, status.Message());
  report->complete = false;
  return Status::Corruption("verification stopped at unsafe persistent bytes");
}

auto PageHeader(PageReader *pages, page_id_t page_id, std::uint64_t visible_lsn, VerifyReport *report,
                const VerifyOptions &options) -> Result<PageHandle> {
  auto page = pages->Read(page_id);
  if (!page) {
    return std::unexpected(Stop(report, options, VerifyIssueKind::Page, page_id, page.error()));
  }
  auto page_lsn = std::uint64_t{0};
  if (const auto *const header = page->ValidatedHeader(); header != nullptr) {
    page_lsn = header->page_lsn;
  } else {
    const auto decoded = storage::DecodeDataPageHeader(
        std::as_bytes(std::span<const char, PAGE_SIZE>{page->Data(), PAGE_SIZE}), page_id);
    if (!decoded) {
      return std::unexpected(Stop(report, options, VerifyIssueKind::Page, page_id, decoded.error()));
    }
    page_lsn = decoded->page_lsn;
  }
  if (page_lsn > visible_lsn) {
    return std::unexpected(Stop(report, options, VerifyIssueKind::Page, page_id,
                                Status::Corruption("page LSN is newer than the verified snapshot")));
  }
  return page;
}

}  // namespace
/*
** SNAPSHOT OWNERSHIP AUDIT
**
** Every data page in the logical page range must belong to exactly one of
** three domains. The range starts at FIRST_DATA_PAGE_ID and ends before
** logical_page_count.
**
**   reachable B+ tree or overflow page
**   reusable allocator extent
**   allocator metadata page
**
** TransactionPages decodes the allocator with the same persistent codec used
** by writers. This full audit walks routing ranges, leaf links, and overflow
** chains, then compares their combined ownership with the allocator domains.
** A complete report accounts for every data page ID below
** logical_page_count. The transaction overlay is never edited or committed.
**
** Keeping this graph-wide proof out of page admission is an architectural
** boundary: ordinary reads validate only the page and edge they consume,
** whereas verification pays for visited and ownership sets to detect aliases,
** leaks, and cycles across the complete snapshot.
*/
auto Snapshot(PageReader *pages, const txn::DatabaseState &state, VerifyOptions options) -> Result<VerifyReport> {
  if (options.max_issues == 0) {
    return std::unexpected(Status::InvalidArgument("verification requires a positive issue limit"));
  }
  auto report = VerifyReport{};
  report.visible_lsn = state.visible_lsn;

  auto transaction = txn::TransactionPages::Begin(pages, state, PAGE_SIZE);
  if (!transaction) {
    if (!IsPersistentFailure(transaction.error())) {
      return std::unexpected(transaction.error());
    }
    AddIssue(&report, options, VerifyIssueKind::Allocator, state.allocator_root_page_id, transaction.error().Message());
    report.complete = false;
    return report;
  }
  const auto &free_extents = transaction->FreeExtents();

  auto free_pages = std::unordered_set<page_id_t>{};
  for (const auto &extent : free_extents) {
    if (extent.retire_lsn <= state.checkpoint_lsn) {
      report.reusable_pages += extent.page_count;
    } else {
      report.retired_pages += extent.page_count;
    }
    for (page_id_t page_id = extent.first_page_id; page_id < extent.first_page_id + extent.page_count; ++page_id) {
      free_pages.insert(page_id);
    }
  }
  const auto allocator_pages =
      std::unordered_set<page_id_t>(transaction->AllocatorPageIds().begin(), transaction->AllocatorPageIds().end());
  report.allocator_pages = allocator_pages.size();

  for (const auto page_id : allocator_pages) {
    if (free_pages.contains(page_id)) {
      AddIssue(&report, options, VerifyIssueKind::DoubleAllocation, page_id,
               "allocator metadata overlaps free storage");
      continue;
    }
    auto page = PageHeader(pages, page_id, state.visible_lsn, &report, options);
    if (!page) {
      return IsPersistentFailure(page.error()) ? Result<VerifyReport>{std::move(report)}
                                               : Result<VerifyReport>{std::unexpected(page.error())};
    }
    ++report.pages_checked;
  }

  using Bound = std::optional<std::string_view>;
  auto visited = std::unordered_set<page_id_t>{};
  auto previous_leaf = std::optional<std::pair<page_id_t, page_id_t>>{};
  const auto less = txn::BytewiseLess{};

  const auto visit = [&](auto &&self, page_id_t page_id, Bound lower, Bound upper) -> Status {
    if (page_id < FIRST_DATA_PAGE_ID || page_id >= state.logical_page_count) {
      return Stop(&report, options, VerifyIssueKind::TreeStructure, page_id,
                  Status::Corruption("tree edge lies outside the logical page range"));
    }
    if (free_pages.contains(page_id) || allocator_pages.contains(page_id)) {
      return Stop(&report, options, VerifyIssueKind::DoubleAllocation, page_id,
                  Status::Corruption("reachable tree page is owned by the allocator"));
    }
    if (!visited.insert(page_id).second) {
      return Stop(&report, options, VerifyIssueKind::DoubleAllocation, page_id,
                  Status::Corruption("tree contains a duplicate page reference or cycle"));
    }

    auto page = PageHeader(pages, page_id, state.visible_lsn, &report, options);
    if (!page) {
      return page.error();
    }
    ++report.pages_checked;
    const auto type = RawNodeType(*page);
    if (type == static_cast<std::uint16_t>(NodeType::Leaf)) {
      const auto leaf = LeafPageView::Open(*page);
      if (!leaf) {
        return Stop(&report, options, VerifyIssueKind::TreeStructure, page_id, leaf.error());
      }
      ++report.leaf_pages;
      for (std::size_t index = 0; index < leaf->Count(); ++index) {
        const auto key = leaf->KeyAt(index);
        if ((lower && less(key, *lower)) || (upper && !less(key, *upper))) {
          return Stop(&report, options, VerifyIssueKind::TreeStructure, page_id,
                      Status::Corruption("leaf key lies outside its parent routing range"));
        }
        const auto value = leaf->ValueAt(index);
        if (value.IsOverflow()) {
          const auto before = visited.size();
          if (auto status = ValidateOverflowValue(pages, value.OverflowDescriptor(), state.logical_page_count,
                                                  state.visible_lsn, free_pages, allocator_pages, &visited);
              !status.Ok()) {
            return Stop(&report, options, VerifyIssueKind::OverflowValue, value.OverflowDescriptor().first_page_id,
                        std::move(status));
          }
          const auto count = visited.size() - before;
          report.overflow_pages += count;
          report.pages_checked += count;
        }
      }
      if (previous_leaf && previous_leaf->second != page_id) {
        AddIssue(&report, options, VerifyIssueKind::LeafLink, previous_leaf->first,
                 "leaf link does not match in-order tree traversal");
      }
      previous_leaf = std::pair{page_id, leaf->NextLeaf()};
      return {};
    }
    if (type != static_cast<std::uint16_t>(NodeType::Internal)) {
      return Stop(&report, options, VerifyIssueKind::TreeStructure, page_id,
                  Status::Corruption("reachable page is not a B+ tree node"));
    }

    const auto node = InternalPageView::Open(*page);
    if (!node) {
      return Stop(&report, options, VerifyIssueKind::TreeStructure, page_id, node.error());
    }
    ++report.internal_pages;
    for (std::size_t child = 0; child <= node->SeparatorCount(); ++child) {
      const auto child_lower = child == 0 ? lower : Bound{node->KeyAt(child - 1)};
      const auto child_upper = child == node->SeparatorCount() ? upper : Bound{node->KeyAt(child)};
      if (child_lower && child_upper && !less(*child_lower, *child_upper)) {
        return Stop(&report, options, VerifyIssueKind::TreeStructure, page_id,
                    Status::Corruption("internal node has an invalid routing range"));
      }
      if (auto status = self(self, node->ChildAt(child), child_lower, child_upper); !status.Ok()) {
        return status;
      }
    }
    return {};
  };

  if (auto status = visit(visit, state.root_page_id, {}, {}); !status.Ok()) {
    return IsPersistentFailure(status) ? Result<VerifyReport>{std::move(report)}
                                       : Result<VerifyReport>{std::unexpected(std::move(status))};
  }

  if (previous_leaf && previous_leaf->second != HEADER_PAGE_ID) {
    AddIssue(&report, options, VerifyIssueKind::LeafLink, previous_leaf->first,
             "leaf link does not match in-order tree traversal");
  }

  for (page_id_t page_id = FIRST_DATA_PAGE_ID; page_id < state.logical_page_count; ++page_id) {
    if (!visited.contains(page_id) && !free_pages.contains(page_id) && !allocator_pages.contains(page_id)) {
      AddIssue(&report, options, VerifyIssueKind::LeakedPage, page_id,
               "allocated page is unreachable and absent from the free index");
    }
  }
  return report;
}

}  // namespace tinydb::verify
