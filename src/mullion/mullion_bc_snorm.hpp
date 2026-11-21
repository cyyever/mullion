#pragma once

namespace mullion {

struct BCSnormFraction {
  int numerator;
  int denominator;
};

// Shared by CPU uploads and Metal: BC4 and each BC5 channel encode the same eight-entry palette.
// Keep cancellation in integers; an exact zero must not acquire the native decoder's interpolation residue.
inline BCSnormFraction decodeBCSnorm(unsigned low, unsigned high, unsigned texel) {
  int a = int(low & 255), b = int((low >> 8) & 255);
  if (a > 127) a -= 256;
  if (b > 127) b -= 256;
  bool eight = a > b;
  if (a < -127) a = -127;
  if (b < -127) b = -127;
  unsigned shift = 16 + texel * 3;
  unsigned index = (shift >= 32 ? high >> (shift - 32) : (low >> shift) | (high << (32 - shift))) & 7;
  if (index < 2)
    return {index ? b : a, 127};
  if (!eight && index >= 6)
    return {index == 6 ? -1 : 1, 1};
  int divisor = eight ? 7 : 5;
  return {(divisor + 1 - int(index)) * a + (int(index) - 1) * b, 127 * divisor};
}

} // namespace mullion
