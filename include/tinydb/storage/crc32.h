#pragma once

/*
 * This is the reflected IEEE CRC-32 used by every persistent
 * TinyDB page and WAL record.
 */

#include <cstdint>
#include <span>

namespace tinydb::storage {

class Crc32Accumulator {
public:
  void Update(std::span<const char> bytes) noexcept;
  [[nodiscard]] auto Finish() const noexcept -> std::uint32_t;

private:
  std::uint32_t remainder_ = 0xFFFFFFFFU;
};

auto Crc32(std::span<const char> bytes) noexcept -> std::uint32_t;

} // namespace tinydb::storage
