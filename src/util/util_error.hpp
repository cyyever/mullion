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

#include <stdexcept>
#include <string>

namespace mullion {

class MTLD3DError : public std::runtime_error {
public:
  MTLD3DError() : std::runtime_error("") {}
  using std::runtime_error::runtime_error;

  std::string message() const { return what(); }
};

} // namespace mullion
