#include <gtest/gtest.h>

#include "txn/database_state.h"
#include "txn/reader_gate.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

/*
** Reader-gate tests force the visibility state machine at thread boundaries:
** shared admission, token copies, publisher fairness, draining, abandoned
** publication, and admission reopening. Atomic flags coordinate only the test;
** correctness under test remains entirely inside ReaderGate.
*/
namespace {

using namespace std::chrono_literals;
using tinydb::txn::DatabaseState;
using tinydb::txn::ReaderGate;

auto State(std::uint64_t version, tinydb::page_id_t root_page_id) -> std::shared_ptr<const DatabaseState> {
  return std::make_shared<const DatabaseState>(DatabaseState{
      .root_page_id = root_page_id,
      .allocator_root_page_id = 11,
      .logical_page_count = 20,
      .visible_lsn = version * 10,
      .checkpoint_lsn = (version - 1) * 10,
  });
}

auto WaitUntil(const auto &predicate) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

TEST(Readers, Snapshot) {
  auto gate = ReaderGate(State(7, 3));
  constexpr auto thread_count = std::size_t{12};
  auto start = std::atomic<bool>{false};
  auto admitted = std::atomic<std::size_t>{0};
  auto release = std::atomic<bool>{false};
  auto failures = std::atomic<std::size_t>{0};
  auto readers = std::vector<std::thread>{};

  for (auto index = std::size_t{0}; index < thread_count; ++index) {
    readers.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      auto snapshot = gate.BeginRead();
      if (snapshot.State().visible_lsn != 70 || snapshot.State().root_page_id != 3) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
      admitted.fetch_add(1, std::memory_order_release);
      while (!release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    });
  }

  start.store(true, std::memory_order_release);
  const auto all_admitted = WaitUntil([&] { return admitted.load(std::memory_order_acquire) == thread_count; });
  EXPECT_TRUE(all_admitted);
  EXPECT_EQ(gate.Stats().active_readers, thread_count);
  EXPECT_TRUE(gate.Stats().oldest_reader_age.has_value());
  release.store(true, std::memory_order_release);
  for (auto &reader : readers) {
    reader.join();
  }
  EXPECT_EQ(failures.load(), 0U);
  EXPECT_EQ(gate.Stats().active_readers, 0U);
  EXPECT_FALSE(gate.Stats().oldest_reader_age.has_value());
}

TEST(Readers, Lifetime) {
  auto gate = ReaderGate(State(1, 3));
  auto cursor_token = tinydb::txn::SnapshotToken{};
  {
    auto transaction_token = gate.BeginRead();
    cursor_token = transaction_token;
    EXPECT_EQ(gate.Stats().active_readers, 1U);
  }

  // Copies of one token share a single admission rather than inflating the
  // reader count for each cursor derived from a transaction.
  EXPECT_EQ(gate.Stats().active_readers, 1U);
  EXPECT_EQ(cursor_token.State().visible_lsn, 10U);
  cursor_token = {};
  EXPECT_EQ(gate.Stats().active_readers, 0U);
}

TEST(Readers, ScopedLifetime) {
  auto gate = ReaderGate(State(3, 7));
  {
    auto snapshot = tinydb::txn::ScopedSnapshotToken{};
    gate.BeginRead(snapshot);
    EXPECT_EQ(snapshot.State().visible_lsn, 30U);
    EXPECT_EQ(snapshot.State().root_page_id, 7U);
    EXPECT_EQ(gate.Stats().active_readers, 1U);
    EXPECT_TRUE(gate.Stats().oldest_reader_age.has_value());

    // Checkpoint publication may advance while readers are active. The scoped
    // admission must retain the exact state it captured, just like a shared
    // transaction token.
    gate.AdvanceCheckpoint(30);
    EXPECT_EQ(snapshot.State().checkpoint_lsn, 20U);
    EXPECT_EQ(gate.CurrentState()->checkpoint_lsn, 30U);
  }
  EXPECT_EQ(gate.Stats().active_readers, 0U);
  EXPECT_FALSE(gate.Stats().oldest_reader_age.has_value());
}

TEST(Readers, ScopedAdmissionRetainsGateState) {
  auto snapshot = tinydb::txn::ScopedSnapshotToken{};
  {
    auto gate = ReaderGate(State(5, 9));
    gate.BeginRead(snapshot);
  }
  EXPECT_EQ(snapshot.State().visible_lsn, 50U);
  EXPECT_EQ(snapshot.State().root_page_id, 9U);
}

TEST(Readers, Fairness) {
  auto gate = ReaderGate(State(1, 3));
  auto old_reader = gate.BeginRead();
  auto publisher_has_gate = std::atomic<bool>{false};
  auto release_publisher = std::atomic<bool>{false};
  auto late_reader_entered = std::atomic<bool>{false};
  auto late_reader_lsn = std::atomic<std::uint64_t>{0};

  auto publisher = std::thread([&] {
    auto publication = gate.BeginPublication();
    publisher_has_gate.store(true, std::memory_order_release);
    while (!release_publisher.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    publication.Publish(State(2, 8));
  });
  ASSERT_TRUE(WaitUntil([&] { return gate.Stats().publication_pending; }));

  auto late_reader = std::thread([&] {
    auto snapshot = gate.BeginRead();
    late_reader_lsn.store(snapshot.State().visible_lsn, std::memory_order_release);
    late_reader_entered.store(true, std::memory_order_release);
  });

  std::this_thread::sleep_for(20ms);
  EXPECT_FALSE(publisher_has_gate.load(std::memory_order_acquire));
  EXPECT_FALSE(late_reader_entered.load(std::memory_order_acquire));

  old_reader = {};
  ASSERT_TRUE(WaitUntil([&] { return publisher_has_gate.load(std::memory_order_acquire); }));
  EXPECT_FALSE(late_reader_entered.load(std::memory_order_acquire));

  release_publisher.store(true, std::memory_order_release);
  publisher.join();
  late_reader.join();
  EXPECT_TRUE(late_reader_entered.load(std::memory_order_acquire));
  EXPECT_EQ(late_reader_lsn.load(std::memory_order_acquire), 20U);
}

TEST(Readers, Abandon) {
  auto gate = ReaderGate(State(4, 5));
  { const auto publication = gate.BeginPublication(); }

  auto reader = gate.BeginRead();
  EXPECT_EQ(reader.State().visible_lsn, 40U);
  EXPECT_FALSE(gate.Stats().publication_pending);
}

}  // namespace
