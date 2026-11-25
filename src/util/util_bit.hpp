/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#pragma once

#include "util_math.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace mullion::bit {

template <size_t Bits> class bitset {
  static constexpr size_t Qwords = align(Bits, 64) / 64;

public:
  constexpr bitset() = default;

  constexpr bool get(uint64_t idx) const {
    uint64_t qword = 0;
    uint64_t bit = idx;

    if constexpr (Qwords > 1) {
      qword = idx / 64;
      bit = idx % 64;
    }

    return (m_qwords[qword] & (1uLL << bit)) != 0;
  }

  constexpr void set(uint64_t idx, bool value) {
    uint64_t qword = 0;
    uint64_t bit = idx;

    if constexpr (Qwords > 1) {
      qword = idx / 64;
      bit = idx % 64;
    }

    if (value)
      m_qwords[qword] |= 1uLL << bit;
    else
      m_qwords[qword] &= ~(1uLL << bit);
  }

  constexpr bool exchange(uint64_t idx, bool value) {
    bool oldValue = get(idx);
    set(idx, value);
    return oldValue;
  }

  constexpr void flip(uint64_t idx) {
    uint64_t qword = 0;
    uint64_t bit = idx;

    if constexpr (Qwords > 1) {
      qword = idx / 64;
      bit = idx % 64;
    }

    m_qwords[qword] ^= 1uLL << bit;
  }

  constexpr void setAll() {
    m_qwords.fill(std::numeric_limits<uint64_t>::max());
    if constexpr (Bits % 64 != 0)
      m_qwords[Qwords - 1] = (1uLL << (Bits % 64)) - 1;
  }

  constexpr void clearAll() { m_qwords.fill(0); }

  constexpr void clearMask(uint32_t mask) { m_qwords[0] &= ~uint64_t(mask); }

  constexpr bool any() const {
    return std::ranges::any_of(m_qwords, [](uint64_t q) { return q != 0; });
  }

  constexpr uint64_t &qword(uint64_t idx) { return m_qwords[idx]; }

  constexpr uint64_t qword(uint64_t idx) const { return m_qwords[idx]; }

  constexpr size_t bitCount() const { return Bits; }

  constexpr size_t qwordCount() const { return Qwords; }

  constexpr bool operator[](uint64_t idx) const { return get(idx); }

  constexpr void setN(uint64_t bits) {
    uint64_t fullqwords = bits / 64;
    uint64_t offset = bits % 64;

    for (size_t i = 0; i < fullqwords; i++)
      m_qwords[i] = std::numeric_limits<uint64_t>::max();

    if (offset > 0)
      m_qwords[fullqwords] = (1uLL << offset) - 1;
  }

private:
  std::array<uint64_t, Qwords> m_qwords{};
};

} // namespace mullion::bit
