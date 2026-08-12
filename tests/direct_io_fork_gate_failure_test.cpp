#include <gtest/gtest.h>

#include "io/direct_io_fork_gate.h"

#include <cerrno>
#include <cstddef>
#include <string>

namespace {

auto RegistrationAttempts() -> std::size_t & {
  static auto attempts = std::size_t{0};
  return attempts;
}

auto RejectAtFork(void (*prepare)(), void (*parent)(), void (*child)()) -> int {
  static_cast<void>(prepare);
  static_cast<void>(parent);
  static_cast<void>(child);
  ++RegistrationAttempts();
  return EAGAIN;
}

TEST(DirectIoForkGate, RegistrationFailureIsSticky) {
  tinydb::io::SetAtForkRegistrarForTest(&RejectAtFork);

  const auto first = tinydb::io::EnsureDirectIoForkGate();
  const auto second = tinydb::io::EnsureDirectIoForkGate();
  const auto operation = tinydb::io::DirectIoOperation::Begin();

  EXPECT_EQ(first.Code(), tinydb::StatusCode::IoError);
  EXPECT_EQ(second.Code(), tinydb::StatusCode::IoError);
  ASSERT_FALSE(operation.has_value());
  EXPECT_EQ(operation.error().Code(), tinydb::StatusCode::IoError);
  EXPECT_NE(first.Message().find("pthread_atfork"), std::string::npos);
  EXPECT_EQ(RegistrationAttempts(), 1U);
  EXPECT_FALSE(tinydb::io::DirectIoForkGateSnapshotForTest().registered);
}

}  // namespace
