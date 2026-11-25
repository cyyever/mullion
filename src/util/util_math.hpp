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

namespace mullion {

template <typename T, typename U = T> constexpr T align(T what, U to) {
  return (what + to - 1) & ~(to - 1);
}

} // namespace mullion
