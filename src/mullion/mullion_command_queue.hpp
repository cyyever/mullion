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
#include "mullion_allocation.hpp"
#include "mullion_capture.hpp"
#include "mullion_command.hpp"
#include "mullion_command_list.hpp"
#include "mullion_context.hpp"
#include "mullion_counter.hpp"
#include "mullion_diag.hpp"
#include "mullion_frame_pacer.hpp"
#include "mullion_query.hpp"
#include "mullion_queue_allocators.hpp"
#include "mullion_queue_profiler.hpp"
#include "mullion_resource_initializer.hpp"
#include "mullion_statistics.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_cpu_fence.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <tuple>
#include <utility>

namespace mullion {

template <typename T> class moveonly_list {
public:
  moveonly_list(T *storage, size_t size) : storage(storage), size_(size) {
    for (unsigned i = 0; i < size_; i++) {
      new (storage + i) T();
    }
  }

  moveonly_list(const moveonly_list &copy) = delete;
  moveonly_list(moveonly_list &&move) {
    storage = move.storage;
    move.storage = nullptr;
    size_ = move.size_;
    move.size_ = 0;
  };

  ~moveonly_list() {
    for (unsigned i = 0; i < size_; i++) {
      storage[i].~T();
    }
  };

  T &
  operator[](int index) {
    return storage[index];
  }

  std::span<T>
  span() const {
    return std::span<T>(storage, size_);
  }

  T *
  data() const {
    return storage;
  }

  size_t
  size() const {
    return size_;
  }

private:
  T *storage;
  size_t size_;
};

class CommandQueue;

class CommandChunk {
public:
  CommandChunk(const CommandChunk &) = delete; // delete copy constructor

  void *allocate_cpu_heap(size_t size, size_t alignment);

  template <CommandWithContext<ArgumentEncodingContext> F>
  void
  emitcc(F &&func) {
    list_enc.emit(std::forward<F>(func), allocate_cpu_heap(list_enc.calculateCommandSize<F>(), 16));
  }

  void
  encode(WMT::CommandBuffer cmdbuf, ArgumentEncodingContext &enc, const bool &cancelled) {
    enc.$$setEncodingContext(chunk_id, frame_);
    auto &statistics = enc.currentFrameStatistics();
    auto t0 = clock::now();
    list_enc.execute(enc);
    if (cancelled)
      return;
    attached_cmdbuf = cmdbuf;
    auto t1 = clock::now();
    readback = diag::profile ? enc.flushCommands<true>(cmdbuf, chunk_id, chunk_event_id)
                             : enc.flushCommands<false>(cmdbuf, chunk_id, chunk_event_id);
    auto t2 = clock::now();
    statistics.encode_prepare_interval += (t1 - t0);
    statistics.encode_flush_interval += (t2 - t1);
  };

  uint64_t chunk_id;
  uint64_t chunk_event_id;
  uint64_t frame_;
  uint64_t signal_frame_latency_fence_;
  std::function<void()> completed;
  QueryReadbacks readback;
  uint64_t resource_initializer_event_id;

private:
  CommandQueue &queue;
  WMT::Reference<WMT::CommandBuffer> attached_cmdbuf;

  CommandList<ArgumentEncodingContext> list_enc;
  AllocationRefTracking ref_tracker;

  friend class CommandQueue;

public:
  explicit CommandChunk(CommandQueue &queue) : queue(queue) { reset(); }

  void
  reset() noexcept {
    signal_frame_latency_fence_ = ~0ull;
    completed = {};
    readback = {};
    list_enc.reset();
    ref_tracker.clear();
    attached_cmdbuf = nullptr;
  }
};

class CommandQueue {
private:
  void CommitChunkInternal(CommandChunk &chunk, uint64_t seq);
  uint32_t EncodingThread();
  uint32_t WaitForFinishThread();

  friend class CommandChunk;
  uint64_t
  GetNextEncoderId() {
    return encoder_seq++;
  }

  // Layer 1: Device & Metal primitives (longest lifetime)
  WMT::Device device;
  WMT::Reference<WMT::CommandQueue> commandQueue;

public:
  WMT::Reference<WMT::SharedEvent> event;

private:
  obj_handle_t shared_event_listener;

  // Layer 2: Long-lived device pools, capture & frame pacing
public:
  InternalCommandLibrary cmd_library;
  CounterPool counter_pool;
  ResourceInitializer initializer;
  diag::TimestampSamples timestamp_samples;

private:
  CaptureState capture_state;
  FramePacer frame_pacer_;

  // Layer 3: Ring-bump allocators & profiler (outlive chunks and argument_encoding_ctx)
  CommandQueueAllocators allocators_;
  CommandQueueProfiler profiler_;

  // Layer 4: Chunk ring & synchronization state
  std::atomic_uint64_t ready_for_encode = 1; // we start from 1, so 0 is always coherent
  std::atomic_uint64_t ready_for_commit = 1;
  std::atomic_uint64_t chunk_ongoing = 0;
  CpuFence cpu_coherent;
  std::atomic_bool stopped = false;
  std::atomic_bool draining_ = false;
  uint64_t drain_last_seq_ = 0;
  bool encoding_cancelled_ = false;
  uint64_t encoder_seq = 1;
  std::array<CommandChunk, kCommandChunkCount> chunks;

public:
  std::uint64_t current_event_seq_id = 0;

  // Layer 5: Stateful encoder context (destroyed before chunks, allocators & pools)
  ArgumentEncodingContext argument_encoding_ctx;

private:
  // Layer 6: Worker threads (declared last: started after all state is ready, joined first)
  mullion::thread event_listener_thread;
  mullion::thread encodeThread;
  mullion::thread finishThread;

public:
  explicit CommandQueue(WMT::Device device);
  ~CommandQueue();

  void Shutdown();
  void Drain();

  void
  WaitSharedEvent(WMT::SharedEvent event, uint64_t value) {
    while (event.signaledValue() < value) {
      if (stopped.load(std::memory_order_acquire) || draining_.load(std::memory_order_acquire)) {
        encoding_cancelled_ = true;
        return;
      }
      event.waitUntilSignaledValue(value, 100);
    }
  }

  void
  OnCompletion(uint64_t seq, std::function<void()> &&action) {
    auto &completed = chunks[seq % kCommandChunkCount].completed;
    completed = [previous = std::move(completed), action = std::move(action)] {
      if (previous)
        previous();
      action();
    };
  }

  CommandChunk *
  CurrentChunk() {
    auto id = ready_for_encode.load(std::memory_order_relaxed);
    return &chunks[id % kCommandChunkCount];
  }

  uint64_t
  CoherentSeqId() {
    return cpu_coherent.signaledValue();
  }

  uint64_t
  CurrentSeqId() {
    return ready_for_encode.load(std::memory_order_relaxed);
  }

  uint64_t
  GetNextEventSeqId() {
    return ++current_event_seq_id;
  }

  uint64_t
  GetCurrentEventSeqId() {
    return current_event_seq_id;
  }

  uint64_t
  SignaledEventSeqId() {
    return event.signaledValue();
  }

  obj_handle_t
  GetSharedEventListener() {
    return shared_event_listener;
  }

  /**
   * This is not thread-safe!
   * CurrentChunk & CommitCurrentChunk should be called on the same thread
   */
  void CommitCurrentChunk();

  WMTSampleBufferAttachmentInfo
  ProfilePass(EncoderData *pass, diag::ProfileBuffer *&buffer) {
    return profiler_.ProfilePass(
        argument_encoding_ctx.currentSeqId(), *pass, argument_encoding_ctx.emulated_cmd, buffer
    );
  }

  FrameStatisticsContainer &
  Statistics() {
    return frame_pacer_.Statistics();
  }

  uint64_t
  CurrentFrameSeq() const {
    return frame_pacer_.CurrentFrameSeq();
  }

  FrameStatistics &
  CurrentFrameStatistics() {
    return frame_pacer_.CurrentFrameStatistics();
  }

  void
  PresentBoundary() {
    frame_pacer_.PresentBoundary();
  }

  uint32_t
  GetMaxLatency() const {
    return frame_pacer_.GetMaxLatency();
  }

  void
  SetMaxLatency(uint32_t value) {
    frame_pacer_.SetMaxLatency(value);
  }

  void
  WaitCPUFence(uint64_t seq) {
    auto began = diag::profile && cpu_coherent.signaledValue() < seq ? diag::Now() : 0;
    cpu_coherent.wait(seq);
    if (began)
      diag::Waited("cpu", began);
  }

  std::tuple<WMT::Buffer, uint64_t>
  AllocateStagingBuffer(size_t size, size_t alignment) {
    return allocators_.AllocateStagingBuffer(ready_for_encode, cpu_coherent.signaledValue(), size, alignment);
  }

  std::pair<WMT::Buffer, uint64_t>
  AllocateTempBuffer(uint64_t seq, size_t size, size_t alignment) {
    return allocators_.AllocateTempBuffer(seq, cpu_coherent.signaledValue(), size, alignment);
  }

  AllocatedTempBufferSlice
  AllocateTempBuffer1(uint64_t seq, size_t size, size_t alignment) {
    return allocators_.AllocateTempBuffer1(seq, cpu_coherent.signaledValue(), size, alignment);
  }

  std::tuple<void *, WMT::Buffer, uint64_t>
  AllocateArgumentBuffer(uint64_t seq, size_t size) {
    return allocators_.AllocateArgumentBuffer(seq, cpu_coherent.signaledValue(), size);
  }

  std::tuple<void *, WMT::Buffer, uint64_t, uint64_t>
  AllocateAddressedArgumentBuffer(uint64_t seq, size_t size) {
    return allocators_.AllocateAddressedArgumentBuffer(seq, cpu_coherent.signaledValue(), size);
  }

  void *
  AllocateCommandData(size_t size, size_t alignment) {
    return allocators_.AllocateCommandData(ready_for_encode, cpu_coherent.signaledValue(), size, alignment);
  }

  void Retain(uint64_t seq, Allocation *allocation);
};

} // namespace mullion
