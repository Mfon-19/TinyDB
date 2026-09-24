// Copyright 2017 The Chromium Authors.
// AVX-512 folding adapted from Chromium third_party/zlib/crc32_simd.c.
// https://chromium.googlesource.com/chromium/src/+/main/third_party/zlib/crc32_simd.c

#include "tinydb/storage/crc32.h"
#include <array>
#include <bit>
#include <cassert>
#include <immintrin.h>

namespace tinydb::storage {
namespace {

auto UpdateBlocks(const char *buf, std::size_t len,
                  std::uint32_t crc) noexcept -> std::uint32_t {
  assert(len >= 256 && len % 64 == 0);
  /*
   * Definitions of the bit-reflected domain constants k1,k2,k3,k4
   * are similar to those given at the end of the paper, and remaining
   * constants and CRC32+Barrett polynomials remain unchanged.
   *
   * Replace the index of x from 128 to 512. As follows:
   * k1 = ( x ^ ( 512 * 4 + 32 ) mod P(x) << 32 )' << 1 = 0x011542778a
   * k2 = ( x ^ ( 512 * 4 - 32 ) mod P(x) << 32 )' << 1 = 0x01322d1430
   * k3 = ( x ^ ( 512 + 32 ) mod P(x) << 32 )' << 1 = 0x0154442bd4
   * k4 = ( x ^ ( 512 - 32 ) mod P(x) << 32 )' << 1 = 0x01c6e41596
   */
  alignas(64) static const std::uint64_t k1k2[] = {
      0x011542778a, 0x01322d1430, 0x011542778a, 0x01322d1430,
      0x011542778a, 0x01322d1430, 0x011542778a, 0x01322d1430};
  alignas(64) static const std::uint64_t k3k4[] = {
      0x0154442bd4, 0x01c6e41596, 0x0154442bd4, 0x01c6e41596,
      0x0154442bd4, 0x01c6e41596, 0x0154442bd4, 0x01c6e41596};
  alignas(16) static const std::uint64_t k5k6[] = {0x01751997d0, 0x00ccaa009e};
  alignas(16) static const std::uint64_t k7k8[] = {0x0163cd6124, 0x0000000000};
  alignas(16) static const std::uint64_t poly[] = {0x01db710641, 0x01f7011641};
  __m512i x0, x1, x2, x3, x4, x5, x6, x7, x8, y5, y6, y7, y8;
  __m128i a0, a1, a2, a3;

  /*
   * There's at least one block of 256.
   */
  x1 = _mm512_loadu_si512(buf + 0x00);
  x2 = _mm512_loadu_si512(buf + 0x40);
  x3 = _mm512_loadu_si512(buf + 0x80);
  x4 = _mm512_loadu_si512(buf + 0xC0);

  x1 = _mm512_xor_si512(
      x1, _mm512_castsi128_si512(_mm_cvtsi32_si128(std::bit_cast<int>(crc))));

  x0 = _mm512_load_si512(k1k2);

  buf += 256;
  len -= 256;

  /*
   * Parallel fold blocks of 256, if any.
   */
  while (len >= 256) {
    x5 = _mm512_clmulepi64_epi128(x1, x0, 0x00);
    x6 = _mm512_clmulepi64_epi128(x2, x0, 0x00);
    x7 = _mm512_clmulepi64_epi128(x3, x0, 0x00);
    x8 = _mm512_clmulepi64_epi128(x4, x0, 0x00);

    x1 = _mm512_clmulepi64_epi128(x1, x0, 0x11);
    x2 = _mm512_clmulepi64_epi128(x2, x0, 0x11);
    x3 = _mm512_clmulepi64_epi128(x3, x0, 0x11);
    x4 = _mm512_clmulepi64_epi128(x4, x0, 0x11);

    y5 = _mm512_loadu_si512(buf + 0x00);
    y6 = _mm512_loadu_si512(buf + 0x40);
    y7 = _mm512_loadu_si512(buf + 0x80);
    y8 = _mm512_loadu_si512(buf + 0xC0);

    x1 = _mm512_xor_si512(x1, x5);
    x2 = _mm512_xor_si512(x2, x6);
    x3 = _mm512_xor_si512(x3, x7);
    x4 = _mm512_xor_si512(x4, x8);

    x1 = _mm512_xor_si512(x1, y5);
    x2 = _mm512_xor_si512(x2, y6);
    x3 = _mm512_xor_si512(x3, y7);
    x4 = _mm512_xor_si512(x4, y8);

    buf += 256;
    len -= 256;
  }

  /*
   * Fold into 512-bits.
   */
  x0 = _mm512_load_si512(k3k4);

  x5 = _mm512_clmulepi64_epi128(x1, x0, 0x00);
  x1 = _mm512_clmulepi64_epi128(x1, x0, 0x11);
  x1 = _mm512_xor_si512(x1, x2);
  x1 = _mm512_xor_si512(x1, x5);

  x5 = _mm512_clmulepi64_epi128(x1, x0, 0x00);
  x1 = _mm512_clmulepi64_epi128(x1, x0, 0x11);
  x1 = _mm512_xor_si512(x1, x3);
  x1 = _mm512_xor_si512(x1, x5);

  x5 = _mm512_clmulepi64_epi128(x1, x0, 0x00);
  x1 = _mm512_clmulepi64_epi128(x1, x0, 0x11);
  x1 = _mm512_xor_si512(x1, x4);
  x1 = _mm512_xor_si512(x1, x5);

  /*
   * Single fold blocks of 64, if any.
   */
  while (len >= 64) {
    x2 = _mm512_loadu_si512(buf);

    x5 = _mm512_clmulepi64_epi128(x1, x0, 0x00);
    x1 = _mm512_clmulepi64_epi128(x1, x0, 0x11);
    x1 = _mm512_xor_si512(x1, x2);
    x1 = _mm512_xor_si512(x1, x5);

    buf += 64;
    len -= 64;
  }

  /*
   * Fold 512-bits to 384-bits.
   */
  a0 = _mm_load_si128(reinterpret_cast<const __m128i *>(k5k6));

  a1 = _mm512_extracti32x4_epi32(x1, 0);
  a2 = _mm512_extracti32x4_epi32(x1, 1);

  a3 = _mm_clmulepi64_si128(a1, a0, 0x00);
  a1 = _mm_clmulepi64_si128(a1, a0, 0x11);

  a1 = _mm_xor_si128(a1, a3);
  a1 = _mm_xor_si128(a1, a2);

  /*
   * Fold 384-bits to 256-bits.
   */
  a2 = _mm512_extracti32x4_epi32(x1, 2);
  a3 = _mm_clmulepi64_si128(a1, a0, 0x00);
  a1 = _mm_clmulepi64_si128(a1, a0, 0x11);
  a1 = _mm_xor_si128(a1, a3);
  a1 = _mm_xor_si128(a1, a2);

  /*
   * Fold 256-bits to 128-bits.
   */
  a2 = _mm512_extracti32x4_epi32(x1, 3);
  a3 = _mm_clmulepi64_si128(a1, a0, 0x00);
  a1 = _mm_clmulepi64_si128(a1, a0, 0x11);
  a1 = _mm_xor_si128(a1, a3);
  a1 = _mm_xor_si128(a1, a2);

  /*
   * Fold 128-bits to 64-bits.
   */
  a2 = _mm_clmulepi64_si128(a1, a0, 0x10);
  a3 = _mm_setr_epi32(~0, 0, ~0, 0);
  a1 = _mm_srli_si128(a1, 8);
  a1 = _mm_xor_si128(a1, a2);

  a0 = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(k7k8));
  a2 = _mm_srli_si128(a1, 4);
  a1 = _mm_and_si128(a1, a3);
  a1 = _mm_clmulepi64_si128(a1, a0, 0x00);
  a1 = _mm_xor_si128(a1, a2);

  /*
   * Barret reduce to 32-bits.
   */
  a0 = _mm_load_si128(reinterpret_cast<const __m128i *>(poly));

  a2 = _mm_and_si128(a1, a3);
  a2 = _mm_clmulepi64_si128(a2, a0, 0x10);
  a2 = _mm_and_si128(a2, a3);
  a2 = _mm_clmulepi64_si128(a2, a0, 0x00);
  a1 = _mm_xor_si128(a1, a2);

  /*
   * Return the crc32.
   */
  return static_cast<std::uint32_t>(_mm_extract_epi32(a1, 1));
}

// Reduce a reflected 128-bit message to its uncomplemented IEEE CRC.
// Constants and reduction are the same as the final folds in UpdateBlocks.
auto Reduce128(__m128i value) noexcept -> std::uint32_t {
  const auto fold = _mm_set_epi64x(0x00ccaa009e, 0x01751997d0);
  const auto mask = _mm_setr_epi32(-1, 0, -1, 0);
  auto product = _mm_clmulepi64_si128(value, fold, 0x10);
  value = _mm_xor_si128(_mm_srli_si128(value, 8), product);
  product = _mm_clmulepi64_si128(_mm_and_si128(value, mask),
                                 _mm_set_epi64x(0, 0x0163cd6124), 0x00);
  value = _mm_xor_si128(_mm_srli_si128(value, 4), product);
  const auto polynomial = _mm_set_epi64x(0x01f7011641, 0x01db710641);
  product = _mm_clmulepi64_si128(_mm_and_si128(value, mask), polynomial, 0x10);
  product =
      _mm_clmulepi64_si128(_mm_and_si128(product, mask), polynomial, 0x00);
  return static_cast<std::uint32_t>(
      _mm_extract_epi32(_mm_xor_si128(value, product), 1));
}

} // namespace

void Crc32Accumulator::Update(std::span<const char> bytes) noexcept {
  if (bytes.size() >= 256) {
    const auto length = bytes.size() & ~std::size_t{63};
    remainder_ = UpdateBlocks(bytes.data(), length, remainder_);
    bytes = bytes.subspan(length);
  }
  // At most three complete 16-byte blocks remain after the wide path.
  while (bytes.size() >= 16) {
    const auto value = _mm_xor_si128(
        _mm_loadu_si128(reinterpret_cast<const __m128i *>(bytes.data())),
        _mm_cvtsi32_si128(std::bit_cast<int>(remainder_)));
    remainder_ = Reduce128(value);
    bytes = bytes.subspan(16);
  }
  if (!bytes.empty()) {
    const auto length = static_cast<unsigned>(bytes.size());
    const auto mask = static_cast<__mmask16>((1U << length) - 1U);
    auto value =
        _mm_xor_si128(_mm_maskz_loadu_epi8(mask, bytes.data()),
                      _mm_cvtsi32_si128(std::bit_cast<int>(remainder_)));
    // Right-align the message so the padding precedes it. Leading zero bytes
    // with a zero CRC state do not affect the result. Negative shuffle indices
    // zero the padding without reading outside the input span.
    const auto indices = _mm_sub_epi8(
        _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15),
        _mm_set1_epi8(static_cast<char>(16U - length)));
    value = _mm_shuffle_epi8(value, indices);
    // For fewer than four bytes, the upper seed bytes shift directly into
    // the result; the shuffle above retains only the seed bytes consumed.
    const auto rest = length < 4 ? remainder_ >> (length * 8U) : 0U;
    remainder_ = Reduce128(value) ^ rest;
  }
}

auto Crc32Accumulator::Finish() const noexcept -> std::uint32_t {
  return remainder_ ^ 0xFFFFFFFFU;
}

auto Crc32(std::span<const char> bytes) noexcept -> std::uint32_t {
  Crc32Accumulator crc;
  crc.Update(bytes);
  return crc.Finish();
}

auto Crc32WithZeroedU32(std::span<const char> bytes,
                        std::size_t offset) noexcept -> std::uint32_t {
  constexpr std::array<char, sizeof(std::uint32_t)> zeros{};
  assert(bytes.size() >= zeros.size());
  assert(offset <= bytes.size() - zeros.size());
  Crc32Accumulator crc;
  crc.Update(bytes.first(offset));
  crc.Update(zeros);
  crc.Update(bytes.subspan(offset + zeros.size()));
  return crc.Finish();
}

} // namespace tinydb::storage
