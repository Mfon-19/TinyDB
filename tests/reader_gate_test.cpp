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

namespace {

using namespace std::chrono_literals;
using tinydb::txn::DatabaseState;
using tinydb::txn::ReaderGate;

auto State(std::uint64_t transaction_id, tinydb::page_id_t root_page_id)
    -> std::shared_ptr<const DatabaseState> {
  return std::make_shared<const DatabaseState>(DatabaseState{
      .root_page_id = root_page_id,
      .allocator_root_page_id = 11,
      .high_water_page_id = 20,
      .transaction_id = transaction_id,
      .visible_lsn = transaction_id * 10,
      .checkpoint_lsn = (transaction_id - 1) * 10,
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

TEST(ReaderGateTest, ManyReadersCaptureOneImmutableState) {
  auto gate = ReaderGate(State(7, 3));
  constexpr auto THREADS = std::size_t{12};
  auto start = std::atomic<bool>{false};
  auto admitted = std::atomic<std::size_t>{0};
  auto release = std::atomic<bool>{false};
  auto failures = std::atomic<std::size_t>{0};
  auto readers = std::vector<std::thread>{};

  for (auto index = std::size_t{0}; index < THREADS; ++index) {
    readers.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      auto snapshot = gate.BeginRead();
      if (snapshot.State().transaction_id != 7 || snapshot.State().root_page_id != 3) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
      admitted.fetch_add(1, std::memory_order_release);
      while (!release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    });
  }

  start.store(true, std::memory_order_release);
  const auto all_admitted = WaitUntil([&] {
    return admitted.load(std::memory_order_acquire) == THREADS;
  });
  EXPECT_TRUE(all_admitted);
  EXPECT_EQ(gate.Stats().active_readers, THREADS);
  EXPECT_TRUE(gate.Stats().oldest_reader_age.has_value());
  release.store(true, std::memory_order_release);
  for (auto &reader : readers) {
    reader.join();
  }
  EXPECT_EQ(failures.load(), 0U);
  EXPECT_EQ(gate.Stats().active_readers, 0U);
  EXPECT_FALSE(gate.Stats().oldest_reader_age.has_value());
}

TEST(ReaderGateTest, SharedTokenOutlivesTransactionWrapper) {
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
  EXPECT_EQ(cursor_token.State().transaction_id, 1U);
  cursor_token = {};
  EXPECT_EQ(gate.Stats().active_readers, 0U);
}

TEST(ReaderGateTest, PendingPublisherDrainsOldReadersAndBlocksNewReaders) {
  auto gate = ReaderGate(State(1, 3));
  auto old_reader = gate.BeginRead();
  auto publisher_has_gate = std::atomic<bool>{false};
  auto release_publisher = std::atomic<bool>{false};
  auto late_reader_entered = std::atomic<bool>{false};
  auto late_reader_transaction = std::atomic<std::uint64_t>{0};

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
    late_reader_transaction.store(snapshot.State().transaction_id,
                                  std::memory_order_release);
    late_reader_entered.store(true, std::memory_order_release);
  });

  std::this_thread::sleep_for(20ms);
  EXPECT_FALSE(publisher_has_gate.load(std::memory_order_acquire));
  EXPECT_FALSE(late_reader_entered.load(std::memory_order_acquire));

  old_reader = {};
  ASSERT_TRUE(WaitUntil([&] {
    return publisher_has_gate.load(std::memory_order_acquire);
  }));
  EXPECT_FALSE(late_reader_entered.load(std::memory_order_acquire));

  release_publisher.store(true, std::memory_order_release);
  publisher.join();
  late_reader.join();
  EXPECT_TRUE(late_reader_entered.load(std::memory_order_acquire));
  EXPECT_EQ(late_reader_transaction.load(std::memory_order_acquire), 2U);
}

TEST(ReaderGateTest, AbandonedPublicationReopensTheOldState) {
  auto gate = ReaderGate(State(4, 5));
  {
    auto publication = gate.BeginPublication();
    EXPECT_EQ(publication.CurrentState()->transaction_id, 4U);
  }

  auto reader = gate.BeginRead();
  EXPECT_EQ(reader.State().transaction_id, 4U);
  EXPECT_FALSE(gate.Stats().publication_pending);
}

}  // namespace
