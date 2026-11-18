/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#include "util_string.hpp"

namespace mullion::str {

std::string fromws(const WCHAR *ws) {
  size_t srcLen = length(ws);
  size_t dstLen = transcodeString<char>(nullptr, 0, ws, srcLen);

  std::string result;
  result.resize_and_overwrite(dstLen, [&](char *data, size_t size) noexcept {
    return transcodeString(data, size, ws, srcLen);
  });

  return result;
}

std::wstring tows(const char *mbs) {
  size_t srcLen = length(mbs);
  size_t dstLen = transcodeString<wchar_t>(nullptr, 0, mbs, srcLen);

  std::wstring result;
  result.resize_and_overwrite(dstLen, [&](wchar_t *data, size_t size) noexcept {
    return transcodeString(data, size, mbs, srcLen);
  });

  return result;
}

} // namespace mullion::str
