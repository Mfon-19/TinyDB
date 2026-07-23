#include <gtest/gtest.h>

#include <tinydb/database.h>

#include "support/test_files.h"
#include "wal/wal_codec.h"

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

/*
** PROCESS-CRASH SWEEPS
**
** A child blocks immediately before each TinyDB filesystem call. The parent
** either permits that call or sends SIGKILL, so no destructor, Close, or test
** cleanup can improve the persisted state. Every boundary is deterministic:
** acknowledged mutations must survive, the mutation in flight may be wholly
** present or absent, and recovery may itself be killed and retried.
*/

namespace {

using Operation = std::pair<std::string, std::optional<std::string>>;
using Model = std::map<std::string, std::string>;

void WriteAll(int fd, const void *data, std::size_t size) {
  const auto *bytes = static_cast<const char *>(data);
  while (size != 0) {
    const auto written = ::write(fd, bytes, size);
    if (written <= 0) {
      ::_exit(121);
    }
    bytes += written;
    size -= static_cast<std::size_t>(written);
  }
}

auto ReadAll(int fd, void *data, std::size_t size) -> bool {
  auto *bytes = static_cast<char *>(data);
  while (size != 0) {
    const auto read = ::read(fd, bytes, size);
    if (read == 0) {
      return false;
    }
    if (read < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    bytes += read;
    size -= static_cast<std::size_t>(read);
  }
  return true;
}

auto Options() -> tinydb::Options {
  auto options = tinydb::Options{};
  options.checkpoint.wal_trigger_bytes = 64U << 20U;
  options.checkpoint.hard_wal_bytes = 128U << 20U;
  return options;
}

auto Workload() -> std::vector<Operation> {
  auto operations = std::vector<Operation>{};
  for (std::size_t index = 0; index < 10; ++index) {
    operations.emplace_back(tinydb::test::Key(index), tinydb::test::Value(index, 700));
  }
  operations.emplace_back(tinydb::test::Key(2), std::nullopt);
  operations.emplace_back(tinydb::test::Key(3), std::nullopt);
  operations.emplace_back(tinydb::test::Key(2), tinydb::test::Value(102, 900));
  return operations;
}

void Apply(Model *model, const Operation &operation) {
  if (operation.second) {
    (*model)[operation.first] = *operation.second;
  } else {
    model->erase(operation.first);
  }
}

auto ReadModel(tinydb::Database *database) -> Model {
  auto model = Model{};
  const auto rows = tinydb::test::Rows(*database);
  if (!rows) {
    return model;
  }
  for (const auto &[key, value] : *rows) {
    model[key] = value;
  }
  return model;
}

auto RunChild(const std::filesystem::path &path, const std::vector<Operation> &operations, std::size_t crash_at,
              bool open_only = false) -> std::tuple<std::size_t, std::size_t, bool> {
  int events[2]{};
  int permits[2]{};
  if (::pipe(events) != 0 || ::pipe(permits) != 0) {
    return {};
  }

  const auto child = ::fork();
  if (child == 0) {
    ::close(events[0]);
    ::close(permits[1]);
    auto calls = std::size_t{0};
    tinydb::io::SetTestHook([&](const tinydb::io::Call &) -> std::optional<tinydb::io::Fault> {
      const auto event = std::array<std::size_t, 2>{1, ++calls};
      WriteAll(events[1], event.data(), sizeof(event));
      char permit = 0;
      if (!ReadAll(permits[0], &permit, 1)) {
        ::_exit(122);
      }
      return std::nullopt;
    });

    auto database = tinydb::Database::Open(path, Options());
    if (database && !open_only) {
      for (std::size_t index = 0; index < operations.size(); ++index) {
        const auto &operation = operations[index];
        const auto status =
            operation.second ? database->Put(operation.first, *operation.second) : database->Delete(operation.first);
        if (!status.Ok()) {
          break;
        }
        const auto acknowledged = std::array<std::size_t, 2>{2, index + 1U};
        WriteAll(events[1], acknowledged.data(), sizeof(acknowledged));
      }
    }
    if (database) {
      static_cast<void>(database->Close());
    }
    const auto done = std::array<std::size_t, 2>{3, calls};
    WriteAll(events[1], done.data(), sizeof(done));
    ::_exit(0);
  }

  ::close(events[1]);
  ::close(permits[0]);
  auto acknowledged = std::size_t{0};
  auto calls = std::size_t{0};
  auto killed = false;
  auto event = std::array<std::size_t, 2>{};
  while (ReadAll(events[0], event.data(), sizeof(event))) {
    if (event[0] == 1) {
      calls = event[1];
      if (calls == crash_at) {
        static_cast<void>(::kill(child, SIGKILL));
        killed = true;
        break;
      }
      constexpr char permit = 1;
      WriteAll(permits[1], &permit, 1);
    } else if (event[0] == 2) {
      acknowledged = event[1];
    } else {
      calls = event[1];
      break;
    }
  }
  ::close(events[0]);
  ::close(permits[1]);
  int status = 0;
  static_cast<void>(::waitpid(child, &status, 0));
  return {acknowledged, calls, killed};
}

}  // namespace

TEST(Crash, Commit) {
  const auto path = tinydb::test::Path("crash_commit");
  const auto base = tinydb::test::Path("crash_commit_base");
  tinydb::test::Remove(path);
  tinydb::test::Remove(base);
  {
    auto database = tinydb::Database::Open(base, Options()).value();
    ASSERT_TRUE(database.Close().Ok());
  }
  const auto operations = Workload();
  tinydb::test::Copy(base, path);
  const auto [all_acknowledged, total_calls, dry_killed] =
      RunChild(path, operations, std::numeric_limits<std::size_t>::max());
  ASSERT_FALSE(dry_killed);
  ASSERT_EQ(all_acknowledged, operations.size());

  for (std::size_t crash_at = 1; crash_at <= total_calls; ++crash_at) {
    SCOPED_TRACE("syscall " + std::to_string(crash_at) + "/" + std::to_string(total_calls));
    tinydb::test::Copy(base, path);
    const auto [acknowledged, ignored, killed] = RunChild(path, operations, crash_at);
    static_cast<void>(ignored);
    ASSERT_TRUE(killed);

    auto before = Model{};
    for (std::size_t index = 0; index < acknowledged; ++index) {
      Apply(&before, operations[index]);
    }
    auto after = before;
    if (acknowledged < operations.size()) {
      Apply(&after, operations[acknowledged]);
    }
    auto database = tinydb::Database::Open(path, Options());
    ASSERT_TRUE(database.has_value()) << database.error().ToString();
    const auto found = ReadModel(&*database);
    EXPECT_TRUE(found == before || found == after);
    ASSERT_TRUE(database->Put("post-recovery", "ok").Ok());
  }
  tinydb::test::Remove(path);
  tinydb::test::Remove(base);
}

TEST(Crash, Recovery) {
  const auto source = tinydb::test::Path("crash_recovery_source");
  const auto snapshot = tinydb::test::Path("crash_recovery_snapshot");
  const auto path = tinydb::test::Path("crash_recovery");
  tinydb::test::Remove(source);
  tinydb::test::Remove(snapshot);
  tinydb::test::Remove(path);
  auto source_database = tinydb::Database::Open(source, Options()).value();
  for (const auto &operation : Workload()) {
    const auto status = operation.second ? source_database.Put(operation.first, *operation.second)
                                         : source_database.Delete(operation.first);
    ASSERT_TRUE(status.Ok());
  }
  const auto expected = ReadModel(&source_database);
  tinydb::test::Copy(source, snapshot);

  tinydb::test::Copy(snapshot, path);
  const auto [ignored_acknowledged, total_calls, dry_killed] =
      RunChild(path, {}, std::numeric_limits<std::size_t>::max(), true);
  static_cast<void>(ignored_acknowledged);
  ASSERT_FALSE(dry_killed);
  for (std::size_t crash_at = 1; crash_at <= total_calls; ++crash_at) {
    SCOPED_TRACE("recovery syscall " + std::to_string(crash_at) + "/" + std::to_string(total_calls));
    tinydb::test::Copy(snapshot, path);
    const auto [acknowledged, calls, killed] = RunChild(path, {}, crash_at, true);
    static_cast<void>(acknowledged);
    static_cast<void>(calls);
    ASSERT_TRUE(killed);
    auto recovered = tinydb::Database::Open(path, Options());
    ASSERT_TRUE(recovered.has_value()) << recovered.error().ToString();
    EXPECT_EQ(ReadModel(&*recovered), expected);
  }
  tinydb::test::Remove(source);
  tinydb::test::Remove(snapshot);
  tinydb::test::Remove(path);
}
