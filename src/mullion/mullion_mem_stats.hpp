#pragma once

#include "log/log.hpp"
#include "util_env.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>

namespace mullion::mem_stats {

enum class Kind {
  Texture,
  LinearTexture,
  Buffer,
};

struct Counters {
  std::atomic<int64_t> count{0};
  std::atomic<int64_t> bytes{0};
};

inline bool
enabled() {
  static const bool on = env::getEnvVar("MULLION_MEM_STATS") == "1";
  return on;
}

inline Counters &
counters(Kind kind) {
  static Counters c[3];
  return c[static_cast<int>(kind)];
}

inline void
maybeLog() {
  using clock = std::chrono::steady_clock;
  static std::atomic<int64_t> last_ns{0};
  int64_t now = clock::now().time_since_epoch().count();
  int64_t prev = last_ns.load(std::memory_order_relaxed);
  constexpr int64_t kIntervalNs = 5'000'000'000LL;
  if (now - prev < kIntervalNs)
    return;
  if (!last_ns.compare_exchange_strong(prev, now, std::memory_order_relaxed))
    return;

  auto &t = counters(Kind::Texture);
  auto &lt = counters(Kind::LinearTexture);
  auto &b = counters(Kind::Buffer);
  WARN("mullion mem-stats: textures=", t.count.load(std::memory_order_relaxed), " (",
       t.bytes.load(std::memory_order_relaxed) / (1024 * 1024), " MB) | linear_textures=",
       lt.count.load(std::memory_order_relaxed), " (",
       lt.bytes.load(std::memory_order_relaxed) / (1024 * 1024), " MB) | buffers=",
       b.count.load(std::memory_order_relaxed), " (",
       b.bytes.load(std::memory_order_relaxed) / (1024 * 1024), " MB)");
}

inline void
add(Kind kind, uint64_t bytes) {
  if (!enabled())
    return;
  auto &c = counters(kind);
  c.count.fetch_add(1, std::memory_order_relaxed);
  c.bytes.fetch_add(static_cast<int64_t>(bytes), std::memory_order_relaxed);
  maybeLog();
}

inline void
remove(Kind kind, uint64_t bytes) {
  if (!enabled())
    return;
  auto &c = counters(kind);
  c.count.fetch_sub(1, std::memory_order_relaxed);
  c.bytes.fetch_sub(static_cast<int64_t>(bytes), std::memory_order_relaxed);
  maybeLog();
}

} // namespace mullion::mem_stats
