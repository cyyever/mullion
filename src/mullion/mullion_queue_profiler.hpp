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

#include "Metal.hpp"
#include "mullion_context.hpp"
#include "mullion_diag.hpp"
#include "mullion_query.hpp"
#include "util_string.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace mullion {

class EmulatedCommandContext;

class CommandQueueProfiler {
public:
  CommandQueueProfiler(WMT::Device device, diag::TimestampSamples &timestamp_samples) :
      device_(device),
      timestamp_samples_(timestamp_samples),
      profile_(diag::profile ? std::make_unique<ProfileQueue>() : nullptr) {}

  bool
  Enabled() const {
    return bool(profile_);
  }

  WMTSampleBufferAttachmentInfo
  ProfilePass(
      uint64_t seq_id, EncoderData *pass, const EmulatedCommandContext &commands, diag::ProfileBuffer *&profiled
  );

  void RecordSubmit(uint64_t chunk_id, uint64_t began, bool has_present);

  void RecordExecuteSpan(uint64_t chunk_id, uint64_t began);

  void RecordEncodeAndCommit(uint64_t seq, uint64_t began);

  void ResolveCompletedChunk(
      uint64_t seq, WMT::CommandBuffer cmdbuf, uint64_t completed, QueryReadbacks &readback
  );

private:
  struct ProfileQueue {
    static inline std::atomic_uint64_t queues{0};
    std::string id = str::format("d3d11:", ++queues);
    std::array<diag::ProfileBuffer, kCommandChunkCount> buffers;
  };

  WMT::Device device_;
  diag::TimestampSamples &timestamp_samples_;
  std::unique_ptr<ProfileQueue> profile_;
};

} // namespace mullion
