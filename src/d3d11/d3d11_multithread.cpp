#include "d3d11_multithread.hpp"
#include "thread.hpp"
#include "util_likely.hpp"
#include <atomic>

namespace mullion {

void d3d11_device_mutex::lock() {
  if (!protected_)
    return;
  uint32_t tid = mullion::this_thread::get_id();
  uint32_t expected = 0;
  while(!owner_.compare_exchange_weak(expected, tid, std::memory_order_acquire)) {
    if (unlikely(expected == tid))
      break;
    // better to spin here
    while (owner_.load(std::memory_order_relaxed)) {
#if (defined(__x86_64__) && !defined(__arm64ec__)) || defined(__i386__)
      __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
      __asm__ __volatile__("yield");
#else
// do nothing
#endif
    }
    expected = 0;
  }
  counter_++;
}
void d3d11_device_mutex::unlock() noexcept {
  if (!protected_)
    return;
  counter_--;
  if (likely(counter_ == 0))
    owner_.store(0, std::memory_order_release);
}

bool d3d11_device_mutex::set_protected(bool Protected) {
  bool ret = protected_;
  protected_ = Protected;
  if (ret && !Protected) {
    counter_ = 0;
    owner_.store(0, std::memory_order_release);
  }
  return ret;
}
bool d3d11_device_mutex::get_protected() { return protected_; }

}; // namespace mullion