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
#include "mullion_ring_bump_allocator.hpp"
#include "thread.hpp"
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>

namespace mullion {

class CommandQueueAllocators {
public:
  explicit CommandQueueAllocators(WMT::Device device) :
      staging_({
          device,
          WMTResourceOptionCPUCacheModeWriteCombined | WMTResourceHazardTrackingModeUntracked |
              WMTResourceStorageModeManaged,
          false,
      }),
      copy_temp_({device, WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate}),
      argbuf_({
          device,
          WMTResourceHazardTrackingModeUntracked | WMTResourceCPUCacheModeWriteCombined | WMTResourceStorageModeShared,
      }),
      cpu_command_({}),
      reftracker_storage_({}) {}

  std::tuple<WMT::Buffer, uint64_t>
  AllocateStagingBuffer(uint64_t seq, uint64_t coherent, size_t size, size_t alignment) {
    auto [block, offset] = staging_.allocate(seq, coherent, size, alignment);
    return {block.buffer, offset};
  }

  std::pair<WMT::Buffer, uint64_t>
  AllocateTempBuffer(uint64_t seq, uint64_t coherent, size_t size, size_t alignment) {
    auto [block, offset] = copy_temp_.allocate(seq, coherent, size, alignment);
    return {block.buffer, offset};
  }

  AllocatedTempBufferSlice
  AllocateTempBuffer1(uint64_t seq, uint64_t coherent, size_t size, size_t alignment) {
    auto [block, offset] = copy_temp_.allocate(seq, coherent, size, alignment);
    return {block.buffer, offset, block.gpu_address};
  }

  std::tuple<void *, WMT::Buffer, uint64_t>
  AllocateArgumentBuffer(uint64_t seq, uint64_t coherent, size_t size) {
    auto [block, offset] = argbuf_.allocate(seq, coherent, size, 64);
    return {ptr_add(block.mapped_address, offset), block.buffer, offset};
  }

  // an argument buffer the GPU reaches by address too: its mapping, buffer, offset and GPU address
  std::tuple<void *, WMT::Buffer, uint64_t, uint64_t>
  AllocateAddressedArgumentBuffer(uint64_t seq, uint64_t coherent, size_t size) {
    auto [block, offset] = argbuf_.allocate(seq, coherent, size, 64);
    return {ptr_add(block.mapped_address, offset), block.buffer, offset, block.gpu_address + offset};
  }

  void *
  AllocateCommandData(uint64_t seq, uint64_t coherent, size_t size, size_t alignment) {
    auto [block, offset] = cpu_command_.allocate(seq, coherent, size, alignment);
    return ptr_add(block.ptr, offset);
  }

  std::pair<void *, size_t>
  AllocateRefTrackerStorage(uint64_t seq, uint64_t coherent) {
    constexpr size_t block_size = decltype(reftracker_storage_)::block_size;
    auto [block, _] = reftracker_storage_.allocate(seq, coherent, block_size, 1);
    return {block.ptr, block_size};
  }

  void
  FreeCommandData(uint64_t coherent) {
    cpu_command_.free_blocks(coherent);
  }

  void
  FreeGpuBlocks(uint64_t coherent) {
    staging_.free_blocks(coherent);
    copy_temp_.free_blocks(coherent);
    argbuf_.free_blocks(coherent);
  }

private:
  RingBumpState<StagingBufferBlockAllocator> staging_;
  RingBumpState<GpuPrivateBufferBlockAllocator> copy_temp_;
  RingBumpState<StagingBufferBlockAllocator, kCommandChunkGPUHeapSize> argbuf_;
  RingBumpState<HostBufferBlockAllocator, kCommandChunkCPUHeapSize, mullion::null_mutex> cpu_command_;
  RingBumpState<HostBufferBlockAllocator, 0x1000 /* 4kB */> reftracker_storage_;
};

} // namespace mullion
