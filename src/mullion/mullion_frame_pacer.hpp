/*
 * Copyright 2026 Feifan He for CodeWeavers
 * Copyright 2026 Marc-Aurel Zent for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#pragma once

#include "mullion_diag.hpp"
#include "mullion_statistics.hpp"
#include "util_cpu_fence.hpp"
#include "util_likely.hpp"
#include <cstdint>

namespace mullion {

class FramePacer {
public:
  // DXGI's default (IDXGIDevice1::SetMaximumFrameLatency)
  static constexpr uint32_t kDefaultLatency = 3;

  uint64_t
  CurrentFrameId() const {
    return frame_count_;
  }

  uint64_t
  CurrentFrameSeq() const {
    return frame_count_ + 1;
  }

  FrameStatisticsContainer &
  Statistics() {
    return statistics_;
  }

  FrameStatistics &
  CurrentFrameStatistics() {
    return statistics_.at(frame_count_);
  }

  void
  PresentBoundary() {
    statistics_.compute(frame_count_);
    frame_count_++;
    statistics_.at(frame_count_).reset();
    // After present N-th frame (N starts from 1), wait for (N - max_latency)-th frame to finish rendering
    if (likely(frame_count_ > max_latency_)) {
      auto t0 = clock::now();
      auto began =
          diag::profile && frame_latency_fence_.signaledValue() < frame_count_ - max_latency_ ? diag::Now() : 0;
      frame_latency_fence_.wait(frame_count_ - max_latency_);
      if (began)
        diag::Waited("latency", began);
      auto t1 = clock::now();
      statistics_.at(frame_count_).present_latency_interval += (t1 - t0);
    }
    statistics_.at(frame_count_).latency = max_latency_;
  }

  void
  SignalFrameComplete(uint64_t frame_seq) {
    frame_latency_fence_.signal(frame_seq);
  }

  uint32_t
  GetMaxLatency() const {
    return max_latency_;
  }

  // 0 is the default again
  void
  SetMaxLatency(uint32_t value) {
    max_latency_ = value ? value : kDefaultLatency;
  }

private:
  CpuFence frame_latency_fence_;
  uint64_t frame_count_ = 0;
  uint32_t max_latency_ = kDefaultLatency;
  FrameStatisticsContainer statistics_;
};

} // namespace mullion
