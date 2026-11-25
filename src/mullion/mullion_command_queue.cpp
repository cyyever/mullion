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

#include "mullion_command_queue.hpp"
#include "Metal.hpp"
#include "mullion_heartbeat.hpp"
#include "util_env.hpp"
#include "util_win32_compat.h"
#include <algorithm>
#include <atomic>
#include <utility>

#define ASYNC_ENCODING 1

namespace mullion {

void *
CommandChunk::allocate_cpu_heap(size_t size, size_t alignment) {
  return queue.AllocateCommandData(size, alignment);
}

namespace {

template <size_t... I>
std::array<CommandChunk, sizeof...(I)>
MakeCommandChunks(CommandQueue &queue, std::index_sequence<I...>) {
  return {((void)I, CommandChunk(queue))...};
}

} // namespace

CommandQueue::CommandQueue(WMT::Device device) :
    device(device),
    commandQueue(device.newCommandQueue(kCommandChunkCount)),
    event(device.newSharedEvent()),
    shared_event_listener(SharedEventListener_create()),
    cmd_library(device),
    counter_pool(device),
    initializer(device),
    allocators_(device),
    profiler_(device, timestamp_samples),
    chunks(MakeCommandChunks(*this, std::make_index_sequence<kCommandChunkCount>{})),
    argument_encoding_ctx(*this, device, cmd_library) {
  event_listener_thread = mullion::thread([this]() { SharedEventListener_start(shared_event_listener); });
  encodeThread = mullion::thread([this]() { EncodingThread(); });
  finishThread = mullion::thread([this]() { WaitForFinishThread(); });
}

CommandQueue::~CommandQueue() {
  Shutdown();
}

void
CommandQueue::Drain() {
  drain_last_seq_ = CurrentSeqId() - 1;
  draining_.store(true, std::memory_order_release);
  ready_for_encode.fetch_add(1, std::memory_order_release);
  ready_for_encode.notify_one();
  encodeThread.join();
  // only committed work has completion callbacks; the blocked chunk and its tail are abandoned, and so not ongoing
  auto committed = ready_for_commit.load(std::memory_order_acquire) - 1;
  WaitCPUFence(committed);
  chunk_ongoing.fetch_sub(drain_last_seq_ - committed, std::memory_order_release);
}

void
CommandQueue::Shutdown() {
  if (stopped.load())
    return;
  // what was committed runs to its end, with its completions
  for (uint64_t ongoing; (ongoing = chunk_ongoing.load(std::memory_order_acquire));)
    chunk_ongoing.wait(ongoing, std::memory_order_acquire);
  if (stopped.exchange(true))
    return;

  ready_for_encode++;
  ready_for_encode.notify_one();
  ready_for_commit++;
  ready_for_commit.notify_one();
  SharedEventListener_destroy(shared_event_listener);
  if (encodeThread.joinable())
    encodeThread.join();
  if (finishThread.joinable())
    finishThread.join();
  if (event_listener_thread.joinable())
    event_listener_thread.join();
  for (auto &chunk : chunks)
    chunk.reset();
  argument_encoding_ctx.clearState();
}

void
CommandQueue::CommitCurrentChunk() {
  auto began = profiler_.Enabled() ? diag::Now() : 0;
  auto chunk_id = ready_for_encode.load(std::memory_order_relaxed);
  auto &chunk = chunks[chunk_id % kCommandChunkCount];
  chunk.chunk_id = chunk_id;
  chunk.chunk_event_id = GetNextEventSeqId();
  chunk.frame_ = frame_pacer_.CurrentFrameId();
  chunk.resource_initializer_event_id = initializer.flushToWait();
  auto &statistics = CurrentFrameStatistics();
  statistics.command_buffer_count++;
  profiler_.RecordSubmit(chunk_id, began, chunk.signal_frame_latency_fence_ != ~0ull);
#if ASYNC_ENCODING
  ready_for_encode.fetch_add(1, std::memory_order_release);
  ready_for_encode.notify_one();

  auto t0 = clock::now();
  chunk_ongoing.wait(kCommandChunkCount - 1, std::memory_order_acquire);
  chunk_ongoing.fetch_add(1, std::memory_order_relaxed);
  auto t1 = clock::now();
  statistics.commit_interval += (t1 - t0);
#else
  CommitChunkInternal(chunk, ready_for_encode.fetch_add(1, std::memory_order_relaxed));
#endif

  allocators_.FreeCommandData(cpu_coherent.signaledValue());
  profiler_.RecordExecuteSpan(chunk_id, began);
}

void
CommandQueue::CommitChunkInternal(CommandChunk &chunk, uint64_t seq) {
  auto pool = WMT::MakeAutoreleasePool();

  capture_state.frame(device, chunk.frame_);

  auto cmdbuf = commandQueue.commandBuffer();
  chunk.attached_cmdbuf = cmdbuf;
  if (chunk.resource_initializer_event_id) {
    cmdbuf.encodeWaitForEvent(initializer.event(), chunk.resource_initializer_event_id);
  }
  auto began = profiler_.Enabled() ? diag::Now() : 0;
  chunk.encode(chunk.attached_cmdbuf, argument_encoding_ctx, encoding_cancelled_);
  if (encoding_cancelled_ || stopped.load(std::memory_order_acquire))
    return;
  profiler_.RecordEncodeAndCommit(seq, began);
  FLIGHT_HEAVY(
      Gpu, "context queue %p: chunk %llu of frame %llu commits as %#llx", this, seq, chunk.frame_, cmdbuf.handle
  );
  cmdbuf.commit();
  heartbeat::Submitted();

  ready_for_commit.fetch_add(1, std::memory_order_release);
  ready_for_commit.notify_one();
}

uint32_t
CommandQueue::EncodingThread() {
#if ASYNC_ENCODING
  env::setThreadName("mullion-encode-thread");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  uint64_t internal_seq = 1;
  while (!stopped.load()) {
    ready_for_encode.wait(internal_seq, std::memory_order_acquire);
    if (stopped.load() || (draining_.load(std::memory_order_acquire) && internal_seq > drain_last_seq_))
      break;
    auto &chunk = chunks[internal_seq % kCommandChunkCount];
    CommitChunkInternal(chunk, internal_seq);
    if (encoding_cancelled_)
      break;
    internal_seq++;
  }
#endif
  return 0;
}

uint32_t
CommandQueue::WaitForFinishThread() {
  env::setThreadName("mullion-finish-thread");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  uint64_t internal_seq = 1;
  while (!stopped.load()) {
    ready_for_commit.wait(internal_seq, std::memory_order_acquire);
    if (stopped.load())
      break;
    auto &chunk = chunks[internal_seq % kCommandChunkCount];
    if (profiler_.Enabled() || chunk.attached_cmdbuf.status() <= WMTCommandBufferStatusScheduled) {
      chunk.attached_cmdbuf.waitUntilCompleted();
    }
    auto completed = profiler_.Enabled() ? diag::Now() : 0;
    FLIGHT_HEAVY(
        Gpu, "context queue %p: chunk %llu completes, status %llu", this, internal_seq,
        uint64_t(chunk.attached_cmdbuf.status())
    );
    if (chunk.attached_cmdbuf.status() == WMTCommandBufferStatusError) {
      ERR("Device error at frame ", chunk.frame_, ": ", chunk.attached_cmdbuf.error().description().getUTF8String());
      // the first: Metal fails the command buffers after a fault too
      static bool dumped;
      if (!std::exchange(dumped, true))
        WMTFlightDump("a command buffer error");
    }
    if (auto logs = chunk.attached_cmdbuf.logs()) {
      for (auto &log : logs.elements()) {
        ERR("Frame ", chunk.frame_, ": ", log.description().getUTF8String());
      }
    }

    profiler_.ResolveCompletedChunk(internal_seq, chunk.attached_cmdbuf, completed, chunk.readback);

    uint64_t omitted = 0;
    for (auto word : chunk.readback.omitted)
      omitted = std::max(omitted, *word);
    if (omitted) {
      auto &bound = argument_encoding_ctx.indirect_geometry_bound;
      bound.store(std::max(bound.load(std::memory_order_relaxed), omitted), std::memory_order_relaxed);
      WARN(
          "Omitted indirect mesh draw (GS) because of too many object threadgroups (", omitted,
          "); later indirect draws reserve parts for this many"
      );
    }
    if (chunk.completed)
      chunk.completed();
    if (chunk.signal_frame_latency_fence_ != ~0ull)
      frame_pacer_.SignalFrameComplete(chunk.signal_frame_latency_fence_);

    chunk.reset();
    cpu_coherent.signal(internal_seq);
    chunk_ongoing.fetch_sub(1, std::memory_order_release);
    chunk_ongoing.notify_one();

    allocators_.FreeGpuBlocks(internal_seq);

    internal_seq++;
  }

  return 0;
}

void
CommandQueue::Retain(uint64_t seq, Allocation *allocation) {
  auto &chunk = chunks[seq % kCommandChunkCount];
  auto &tracker = chunk.ref_tracker;
  while (unlikely(!tracker.track(allocation))) {
    auto [storage, block_size] = allocators_.AllocateRefTrackerStorage(seq, cpu_coherent.signaledValue());
    tracker.addStorage(storage, block_size);
  }
}

} // namespace mullion
