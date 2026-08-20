/*
 * This is the interface with Linux that handles the initial
 * creation of the database file and subsequent reading and
 * writing to the file
 */

#include "tinydb/status.h"
#include <cstddef>
#include <string_view>

namespace tinydb::storage {

class DiskManager {
public:
  static Result<DiskManager> Open(std::string_view name);

  ~DiskManager();

  DiskManager(const DiskManager &) = delete;
  DiskManager &operator=(const DiskManager &) = delete;

  DiskManager(DiskManager &&other) noexcept;
  DiskManager &operator=(DiskManager &&other) noexcept;

  Status Write(const std::string_view buffer);
  Status Read(std::string &buffer);

private:
  explicit DiskManager(int fd) noexcept : fd_(fd) {}
  int fd_; // the file descriptor we get from linux
};
} // namespace tinydb::storage