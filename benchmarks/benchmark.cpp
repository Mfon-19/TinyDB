#include "tinydb/database.h"
#include <algorithm>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr unsigned SEED = 42;
constexpr std::string_view HELP =
    "Usage: tinydb_bench DIRECTORY WORKLOAD [OPTIONS]\n"
    "Workloads: all, write, read, scan, churn, concurrent\n"
    "Options (positive integers):\n"
    "  --keys N        Keys in the database (default 10000)\n"
    "  --pool N        Buffer pool capacity in pages (default 256)\n"
    "  --batch N       Writes per transaction (default 100)\n"
    "  --runs N        Independent repetitions (default 3)\n"
    "  --readers N     Concurrent reader threads (default 4)\n"
    "  --value-size N  Value size in bytes (default 100)\n"
    "Creates temporary databases under DIRECTORY; prints CSV to stdout.\n";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "benchmark failed: " << message << '\n';
  std::exit(1);
}

void Check(tinydb::Status status) {
  if (!status.Ok()) {
    Fail(status.Message());
  }
}

template <typename T> T Take(tinydb::Result<T> result) {
  if (!result) {
    Fail(result.error().Message());
  }
  return std::move(*result);
}

struct Options {
  std::string directory;
  std::string workload;
  std::uint32_t keys = 10000;
  std::uint32_t pool = 256;
  std::uint32_t batch = 100;
  std::uint32_t runs = 3;
  std::uint32_t readers = 4;
  std::uint32_t value_size = 100;
};

Options Parse(int argc, char **argv) {
  if (argc < 3 || (argc - 3) % 2 != 0) {
    Fail(HELP);
  }
  Options options{argv[1], argv[2]};
  const std::vector<std::string_view> workloads{"all",  "write", "read",
                                                "scan", "churn", "concurrent"};
  if (std::ranges::find(workloads, options.workload) == workloads.end()) {
    Fail("unknown workload; use --help");
  }
  for (int index = 3; index < argc; index += 2) {
    const std::string_view name = argv[index];
    const std::string_view text = argv[index + 1];
    std::uint32_t value = 0;
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value == 0) {
      Fail(std::format("{} requires a positive integer", name));
    }
    if (name == "--keys") {
      options.keys = value;
    } else if (name == "--pool") {
      options.pool = value;
    } else if (name == "--batch") {
      options.batch = value;
    } else if (name == "--runs") {
      options.runs = value;
    } else if (name == "--readers") {
      options.readers = value;
    } else if (name == "--value-size") {
      options.value_size = value;
    } else {
      Fail(std::format("unknown option: {}", name));
    }
  }
  if (options.value_size > tinydb::MAX_ENTRY_SIZE - 16) {
    Fail(std::format("16-byte keys leave at most {} bytes for values",
                     tinydb::MAX_ENTRY_SIZE - 16));
  }
  return options;
}

struct Data {
  std::vector<std::string> keys;
  std::vector<std::string> missing;
  std::vector<std::size_t> sequential;
  std::vector<std::size_t> random;
  std::string value;
  std::string updated;

  explicit Data(const Options &options)
      : sequential(options.keys), value(options.value_size, 'v'),
        updated(options.value_size, 'w') {
    keys.reserve(options.keys);
    missing.reserve(options.keys);
    for (std::uint64_t index = 0; index < options.keys; ++index) {
      keys.push_back(std::format("{:016x}", 2 * index));
      missing.push_back(std::format("{:016x}", 2 * index + 1));
    }
    std::iota(sequential.begin(), sequential.end(), std::size_t{0});
    random = sequential;
    std::mt19937 generator(SEED);
    std::shuffle(random.begin(), random.end(), generator);
  }
};

struct Measurements {
  std::vector<double> latency_us;
  std::uint64_t operations = 0;
  double seconds = 0;
  double checkpoint_ms = 0;

  explicit Measurements(std::size_t transactions) {
    latency_us.reserve(transactions);
  }

  void Record(Clock::time_point start, std::size_t count) {
    const auto elapsed = Clock::now() - start;
    latency_us.push_back(
        std::chrono::duration<double, std::micro>(elapsed).count());
    operations += count;
  }
};

void Report(std::string_view name, std::uint64_t run, Measurements result,
            const std::string &path) {
  std::ranges::sort(result.latency_us);
  const auto percentile = [&](std::size_t percent) {
    return result.latency_us[(result.latency_us.size() - 1) * percent / 100];
  };
  std::cout << std::format(
                   "{},{},{},{},{:.6f},{:.1f},{:.1f},{:.2f},{:.2f},{:.2f},{:."
                   "2f},{:.3f},{}\n",
                   name, run, result.operations, result.latency_us.size(),
                   result.seconds,
                   static_cast<double>(result.operations) / result.seconds,
                   static_cast<double>(result.latency_us.size()) /
                       result.seconds,
                   percentile(50), percentile(95), percentile(99),
                   result.latency_us.back(), result.checkpoint_ms,
                   std::filesystem::file_size(path))
            << std::flush;
}

void Verify(tinydb::Database &database, const Data &data,
            std::string_view expected) {
  auto reader = Take(database.BeginRead());
  auto cursor = Take(reader->Seek(""));
  for (const auto &key : data.keys) {
    if (!cursor.Valid() || cursor.Key() != key || cursor.Value() != expected) {
      Fail("stored contents do not match the workload");
    }
    Check(cursor.Next());
  }
  if (cursor.Valid()) {
    Fail("unexpected keys after the workload");
  }
}

void Write(tinydb::Database &database, const Data &data, Measurements &result,
           std::span<const std::size_t> order, std::size_t batch,
           std::string_view value, bool remove = false) {
  const auto start = Clock::now();
  for (std::size_t offset = 0; offset < order.size();) {
    const auto count = std::min(batch, order.size() - offset);
    const auto transaction_start = Clock::now();
    {
      auto writer = Take(database.BeginWrite());
      for (const auto index : order.subspan(offset, count)) {
        if (remove) {
          if (!Take(writer->Delete(data.keys[index]))) {
            Fail("deleting an existing key returned false");
          }
        } else {
          Check(writer->Put(data.keys[index], value));
        }
      }
      Check(writer->Commit());
    }
    result.Record(transaction_start, count);
    offset += count;
  }
  result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
}

double Checkpoint(tinydb::Database &database) {
  const auto start = Clock::now();
  Check(database.Checkpoint());
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}

void Read(tinydb::Database &database, const Data &data, Measurements &result,
          bool missing = false, bool concurrent = false,
          std::size_t rotation = 0) {
  const auto &keys = missing ? data.missing : data.keys;
  const auto start = Clock::now();
  for (std::size_t offset = 0; offset < keys.size(); ++offset) {
    const auto index = data.random[(offset + rotation) % keys.size()];
    const auto transaction_start = Clock::now();
    {
      auto reader = Take(database.BeginRead());
      auto value = Take(reader->Get(keys[index]));
      if (missing ? value.has_value()
                  : !value || (*value != data.value &&
                               (!concurrent || *value != data.updated))) {
        Fail("read returned an unexpected value");
      }
    }
    result.Record(transaction_start, 1);
  }
  result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
}

Measurements Scan(tinydb::Database &database, const Data &data, bool full) {
  const auto length =
      full ? data.keys.size() : std::min<std::size_t>(100, data.keys.size());
  const auto scans =
      full ? 1 : std::max<std::size_t>(1, data.keys.size() / length);
  Measurements result(scans);
  const auto start = Clock::now();
  for (std::size_t scan = 0; scan < scans; ++scan) {
    const auto first =
        full ? 0 : data.random[scan] % (data.keys.size() - length + 1);
    const auto transaction_start = Clock::now();
    {
      auto reader = Take(database.BeginRead());
      auto cursor = Take(reader->Seek(data.keys[first]));
      for (std::size_t index = first; index < first + length; ++index) {
        if (!cursor.Valid() || cursor.Key() != data.keys[index] ||
            cursor.Value() != data.value) {
          Fail("scan returned an unexpected entry");
        }
        Check(cursor.Next());
      }
      if (full && cursor.Valid()) {
        Fail("full scan returned extra entries");
      }
    }
    result.Record(transaction_start, length);
  }
  result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  return result;
}

void Concurrent(tinydb::Database &database, const Data &data,
                const Options &options, std::uint64_t run,
                const std::string &path) {
  std::vector<Measurements> results;
  for (std::size_t index = 0; index < options.readers; ++index) {
    results.emplace_back(data.keys.size());
  }
  Measurements writes((data.keys.size() - 1) / options.batch + 1);
  Clock::time_point start;
  std::barrier ready(static_cast<std::ptrdiff_t>(options.readers) + 1,
                     [&]() noexcept { start = Clock::now(); });
  std::vector<std::jthread> workers;
  for (std::size_t index = 0; index < options.readers; ++index) {
    workers.emplace_back([&, index] {
      ready.arrive_and_wait();
      Read(database, data, results[index], false, true,
           index * (data.keys.size() / options.readers));
    });
  }
  ready.arrive_and_wait();
  Write(database, data, writes, data.random, options.batch, data.updated);
  workers.clear();
  const auto seconds =
      std::chrono::duration<double>(Clock::now() - start).count();
  Measurements reads(0);
  for (auto &result : results) {
    reads.operations += result.operations;
    reads.latency_us.insert(reads.latency_us.end(), result.latency_us.begin(),
                            result.latency_us.end());
  }
  reads.seconds = writes.seconds = seconds;
  writes.checkpoint_ms = Checkpoint(database);
  Verify(database, data, data.updated);
  Report("concurrent_read", run, std::move(reads), path);
  Report("concurrent_write", run, std::move(writes), path);
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << HELP;
    return 0;
  }
  const auto options = Parse(argc, argv);
  const Data data(options);
  std::error_code error;
  std::filesystem::create_directories(options.directory, error);
  if (error) {
    Fail(error.message());
  }
  auto directory =
      (std::filesystem::path(options.directory) / "tinydb-bench-XXXXXX")
          .string();
  if (mkdtemp(directory.data()) == nullptr) {
    Fail("cannot create benchmark directory");
  }
  const auto path = directory + "/database";
  std::cout << std::format(
      "# directory={}, compiler={}, seed={}, keys={}, key_bytes=16, "
      "value_bytes={}, pool_pages={}, batch={}, runs={}, readers={}\n",
      std::filesystem::absolute(directory).string(), __VERSION__, SEED,
      options.keys, options.value_size, options.pool, options.batch,
      options.runs, options.readers);
#ifndef NDEBUG
  std::cout
      << "# Debug build: use -DCMAKE_BUILD_TYPE=Release for measurements.\n";
#endif
  std::cout
      << "workload,run,operations,transactions,seconds,ops_per_sec,txn_per_sec,"
         "p50_us,p95_us,p99_us,max_us,checkpoint_ms,database_bytes\n";

  for (std::uint64_t run = 1; run <= options.runs; ++run) {
    for (const std::string_view name :
         {"write_seq", "write_random", "read", "scan", "churn", "concurrent"}) {
      if (options.workload != "all" && options.workload != name &&
          !(options.workload == "write" && name.starts_with("write_"))) {
        continue;
      }
      auto database = Take(tinydb::Database::Open(path, options.pool));
      Measurements result(data.keys.size());
      if (name.starts_with("write_")) {
        const auto &order = name == "write_seq" ? data.sequential : data.random;
        Write(*database, data, result, order, options.batch, data.value);
        result.checkpoint_ms = Checkpoint(*database);
        Verify(*database, data, data.value);
        Report(name, run, std::move(result), path);
      } else {
        Write(*database, data, result, data.sequential, 1000, data.value);
        Check(database->Checkpoint());
        Verify(*database, data, data.value);
        result = Measurements(data.keys.size());
        if (name == "read") {
          Read(*database, data, result);
          Report("read_hit", run, std::move(result), path);
          result = Measurements(data.keys.size());
          Read(*database, data, result, true);
          Report("read_miss", run, std::move(result), path);
        } else if (name == "scan") {
          Report("scan_100", run, Scan(*database, data, false), path);
          Report("scan_full", run, Scan(*database, data, true), path);
        } else if (name == "churn") {
          Write(*database, data, result, data.random, options.batch,
                data.updated);
          result.checkpoint_ms = Checkpoint(*database);
          Verify(*database, data, data.updated);
          Report("overwrite", run, std::move(result), path);
          const auto removed = std::span{data.random}.first(
              std::max<std::size_t>(1, data.keys.size() / 4));
          result = Measurements(removed.size());
          Write(*database, data, result, removed, options.batch, {}, true);
          result.checkpoint_ms = Checkpoint(*database);
          for (const auto index : removed) {
            if (Take(database->Get(data.keys[index]))) {
              Fail("deleted key is still present");
            }
          }
          Report("delete", run, std::move(result), path);
          result = Measurements(removed.size());
          Write(*database, data, result, removed, options.batch, data.updated);
          result.checkpoint_ms = Checkpoint(*database);
          Verify(*database, data, data.updated);
          Report("reinsert", run, std::move(result), path);
        } else {
          Concurrent(*database, data, options, run, path);
        }
      }
      database.reset();
      std::filesystem::remove(path);
      std::filesystem::remove(path + "-wal");
    }
  }
  std::filesystem::remove(directory);
}
