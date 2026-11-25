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

#include "mullion_queue_profiler.hpp"
#include "mullion_command.hpp"
#include "util_win32_compat.h"
#include <vector>

namespace mullion {

WMTSampleBufferAttachmentInfo
CommandQueueProfiler::ProfilePass(
    uint64_t seq_id, EncoderData &pass, const EmulatedCommandContext &commands, diag::ProfileBuffer *&profiled
) {
  auto &buffer = profile_->buffers[seq_id % kCommandChunkCount];
  buffer.presents += pass.type == EncoderType::Present;
  buffer.timestamps += pass.type == EncoderType::SampleTimestamp;
  const char *kind = pass.type == EncoderType::Render            ? "Render"
                     : pass.type == EncoderType::Compute         ? "Compute"
                     : pass.type == EncoderType::Blit            ? "Blit"
                     : pass.type == EncoderType::Clear           ? "Clear"
                     : pass.type == EncoderType::Resolve         ? "Resolve"
                     : pass.type == EncoderType::SpatialUpscale  ? "SpatialUpscale"
                     : pass.type == EncoderType::TemporalUpscale ? "TemporalUpscale"
                                                                 : nullptr;
  if (!kind)
    return {};
  auto seq = pass.id;
  std::string runs, targets;
  auto target = [&](const char *binding, unsigned slot, WMT::Texture texture, unsigned level, unsigned slice) {
    if (texture)
      targets += diag::Line(
          "target", profile_->id, seq_id, seq, binding, slot, texture.handle, texture.width(), texture.height(),
          uint64_t(texture.pixelFormat()), level, slice
      );
  };
  auto run = [&](uint64_t pipeline) {
    if (!runs.empty())
      runs += '|';
    runs += str::format("Render:", pipeline, "*1*1*0");
  };
  if (pass.type == EncoderType::Render) {
    auto &data = static_cast<RenderEncoderData &>(pass);
    for (unsigned i = 0; i < data.colors.size(); i++) {
      auto &color = data.colors[i];
      target("color", i, color.attachment.texture(), color.level, color.slice);
      target("resolve", i, color.resolve_attachment.texture(), color.resolve_level, color.resolve_slice);
    }
    target("depth", 0, data.depth.attachment.texture(), data.depth.level, data.depth.slice);
    target("stencil", 0, data.stencil.attachment.texture(), data.stencil.level, data.stencil.slice);
    if (!data.draw_auto_marshal_tasks.empty())
      run(commands.draw_auto_arguments_marshal);
    if (!data.gs_arg_marshal_tasks.empty())
      run(commands.gs_draw_arguments_marshal);
    if (!data.ts_arg_marshal_tasks.empty())
      run(commands.ts_draw_arguments_marshal);
    auto drawn = diag::PipelineRuns(data.cmd_head, [](uint64_t pipeline) { return str::format("Render:", pipeline); });
    if (!runs.empty() && !drawn.empty())
      runs += '|';
    runs += drawn;
  } else if (pass.type == EncoderType::Compute) {
    runs = diag::PipelineRuns(static_cast<ComputeEncoderData &>(pass).cmd_head, [](uint64_t pipeline) {
      return str::format("Compute:", pipeline);
    });
  } else if (pass.type == EncoderType::Clear) {
    auto &data = static_cast<ClearEncoderData &>(pass);
    target(data.clear_dsv ? "depth-stencil" : "color", 0, data.attachment.texture(), 0, 0);
  } else if (pass.type == EncoderType::Resolve) {
    auto &data = static_cast<ResolveEncoderData &>(pass);
    target("color", 0, data.src.texture(), 0, 0);
    target("resolve", 0, data.dst.texture(), 0, 0);
  } else if (pass.type == EncoderType::SpatialUpscale) {
    auto &data = static_cast<SpatialUpscaleData &>(pass);
    target("input", 0, data.backbuffer, 0, 0);
    target("output", 0, data.upscaled, 0, 0);
  } else if (pass.type == EncoderType::TemporalUpscale) {
    auto &data = static_cast<TemporalUpscaleData &>(pass);
    target("input", 0, data.input, 0, 0);
    target("output", 0, data.output, 0, 0);
    target("depth", 0, data.depth, 0, 0);
    target("motion", 0, data.motion_vector, 0, 0);
    target("exposure", 0, data.exposure, 0, 0);
  }
  const uint32_t count = pass.type == EncoderType::Render ? WMTRenderTimestampSamples : WMTTimestampSamplesPerStage;
  if (buffer.samples.empty() || count > timestamp_samples_.limit - buffer.samples.back().used)
    buffer.samples.push_back({.buffer = timestamp_samples_.Take(device_, false)});
  auto &samples = buffer.samples.back();
  uint32_t first = ~0u;
  WMTSampleBufferAttachmentInfo attachment{};
  if (samples.buffer && count <= timestamp_samples_.limit - samples.used) {
    first = samples.used;
    samples.used += count;
    attachment = {
        .sample_buffer = samples.buffer,
        .start_of_encoder_sample_index = first,
        .end_of_encoder_sample_index = first + count - 1
    };
  }
  buffer.passes.push_back(
      {.id = seq,
       .encoding = diag::Now(),
       .kind = kind,
       .runs = std::move(runs),
       .targets = std::move(targets),
       .buffer = uint32_t(buffer.samples.size() - 1),
       .first = first,
       .count = count,
       .event = pass.event_path ? pass.event_path : ""}
  );
  profiled = &buffer;
  return attachment;
}

void
CommandQueueProfiler::RecordSubmit(uint64_t chunk_id, uint64_t began, bool has_present) {
  if (!profile_)
    return;
  profile_->buffers[chunk_id % kCommandChunkCount].submitted = began;
  if (has_present)
    diag::Frame(device_, profile_->id, chunk_id);
}

void
CommandQueueProfiler::RecordExecuteSpan(uint64_t chunk_id, uint64_t began) {
  if (!profile_)
    return;
  diag::Write(diag::Line("span", "execute", chunk_id, GetCurrentThreadId(), began, diag::Now() - began));
}

void
CommandQueueProfiler::RecordEncodeAndCommit(uint64_t seq, uint64_t began) {
  if (!profile_)
    return;
  diag::Write(diag::Line("span", "encode", seq, GetCurrentThreadId(), began, diag::Now() - began));
  auto &buffer = profile_->buffers[seq % kCommandChunkCount];
  device_.sampleTimestamps(buffer.cpu_begin, buffer.gpu_begin);
  buffer.committed = diag::Now();
}

void
CommandQueueProfiler::ResolveCompletedChunk(
    uint64_t seq, WMT::CommandBuffer cmdbuf, uint64_t completed, QueryReadbacks &readback
) {
  if (!profile_)
    return;
  auto &buffer = profile_->buffers[seq % kCommandChunkCount];
  uint64_t cpu_end, gpu_end;
  device_.sampleTimestamps(cpu_end, gpu_end);
  std::vector<std::vector<uint64_t>> clocks;
  for (auto &samples : buffer.samples) {
    auto &clock = clocks.emplace_back(samples.used, ~uint64_t(0));
    if (samples.buffer)
      samples.buffer.resolveCounterRange(0, samples.used, clock.data(), clock.size() * sizeof(uint64_t));
  }
  auto lines = diag::Line(
      "cmdbuf", profile_->id, seq, buffer.committed, completed, cmdbuf.gpuStartTime(), cmdbuf.gpuEndTime(),
      buffer.submitted, buffer.passes.size(), buffer.presents, buffer.timestamps
  );
  lines += diag::Line("calibration", profile_->id, seq, buffer.cpu_begin, buffer.gpu_begin, cpu_end, gpu_end);
  for (auto &pass : buffer.passes) {
    auto line = diag::Line("pass", profile_->id, seq, pass.id, pass.kind, pass.encoding, pass.runs);
    line.pop_back();
    for (uint32_t i = 0; i < pass.count; i++)
      line += str::format('\t', pass.first == ~0u ? ~uint64_t(0) : clocks[pass.buffer][pass.first + i]);
    lines += line + '\n' + pass.targets;
    if (!pass.event.empty())
      lines += diag::Line("event", profile_->id, seq, pass.id, pass.event);
  }
  diag::Write(lines);
  readback.timestamp.reset();
  for (auto &samples : buffer.samples)
    if (samples.buffer)
      timestamp_samples_.Return(std::move(samples.buffer));
  buffer = {};
}

} // namespace mullion
