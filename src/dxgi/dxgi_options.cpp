#include "dxgi_options.hpp"
#include <algorithm>

namespace mullion {

static int32_t parsePciId(const std::string &str) {
  if (str.size() != 4)
    return -1;

  int32_t id = 0;

  for (char i : str) {
    id *= 16;

    if (i >= '0' && i <= '9')
      id += i - '0';
    else if (i >= 'A' && i <= 'F')
      id += i - 'A' + 10;
    else if (i >= 'a' && i <= 'f')
      id += i - 'a' + 10;
    else
      return -1;
  }

  return id;
}

DxgiOptions::DxgiOptions(const Config &config) {
  // Fetch these as a string representing a hexadecimal number and parse it.
  this->customVendorId =
      parsePciId(config.getOption<std::string>("dxgi.customVendorId"));
  this->customDeviceId =
      parsePciId(config.getOption<std::string>("dxgi.customDeviceId"));
  this->customDeviceDesc =
      config.getOption<std::string>("dxgi.customDeviceDesc", "");
  this->forceSDR = config.getOption<bool>("dxgi.forceSDR", false);
  this->maxDeviceMemory = uint64_t(std::max(config.getOption<int32_t>("dxgi.maxDeviceMemory", 0), 0)) << 20;
}

} // namespace mullion
