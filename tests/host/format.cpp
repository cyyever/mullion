// contract: str::format writes a byte as its number, as it writes every other integer (a sample count of 8 is "8",
// not the character 8, backspace), and a char still as its character
#include "../../src/util/util_string.hpp"
#include <cstdio>

int
main() {
  using mullion::str::format;
  unsigned failures = 0;
  auto check = [&](const std::string &got, const std::string &want) {
    if (got != want)
      failures++, std::printf("got \"%s\", want \"%s\"\n", got.c_str(), want.c_str());
  };
  for (unsigned value : {0u, 8u, 65u, 255u}) {
    check(format(uint8_t(value)), std::to_string(value));
    check(format("x", uint8_t(value), 'y'), "x" + std::to_string(value) + "y");
  }
  check(format('a', "b"), "ab");
  check(format(std::string(256, 'x')), std::string(256, 'x'));
  const WCHAR empty[] = {0};
  std::basic_string<WCHAR> unicode = {'A', 0x4e2d};
  if constexpr (sizeof(WCHAR) == 2)
    unicode.append({WCHAR(0xd83d), WCHAR(0xde00)});
  else
    unicode.push_back(WCHAR(0x1f600));
  check(mullion::str::fromws(empty), "");
  check(mullion::str::fromws(unicode.c_str()), "A\xe4\xb8\xad\xf0\x9f\x98\x80");
  if (mullion::str::tows("") != L"" ||
      mullion::str::tows("A\xe4\xb8\xad\xf0\x9f\x98\x80") != L"A\u4e2d\U0001f600")
    failures++, std::puts("UTF-8 to wide conversion failed");
  std::printf("%s: %u wrong results\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
