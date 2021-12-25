// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mem/block_allocator.h"

#include <cstddef>
#include <cstdint>

#include "mem/mpmc_queue.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace mem {

BlockAllocator::BlockAllocator(void* memory,
                               const size_t block_size,
                               const size_t num_blocks,
                               decltype(kAlreadyInitialized))
    : block_size_(block_size),
      num_blocks_(num_blocks),
      free_indices_(memory, num_blocks, IndexQueue::kAlreadyInitialized),
      first_block_(reinterpret_cast<uint8_t*>(memory) + queue_size()) {}

BlockAllocator::BlockAllocator(void* memory,
                               const size_t block_size,
                               const size_t num_blocks,
                               decltype(kInitialize))
    : block_size_(block_size),
      num_blocks_(num_blocks),
      free_indices_(memory, num_blocks, IndexQueue::kInitializeData),
      first_block_(reinterpret_cast<uint8_t*>(memory) + queue_size()) {
  for (size_t i = 0; i < num_blocks; ++i) {
    const bool ok = free_indices_.Push(i);
    ABSL_ASSERT(ok);
  }
}

BlockAllocator::BlockAllocator(absl::Span<uint8_t> region,
                               size_t block_size,
                               decltype(kAlreadyInitialized))
    : BlockAllocator(region.data(),
                     block_size,
                     ComputeMaximumCapacity(region.size(), block_size),
                     kAlreadyInitialized) {}

BlockAllocator::BlockAllocator(absl::Span<uint8_t> region,
                               size_t block_size,
                               decltype(kInitialize))
    : BlockAllocator(region.data(),
                     block_size,
                     ComputeMaximumCapacity(region.size(), block_size),
                     kInitialize) {}

BlockAllocator::~BlockAllocator() = default;

// static
size_t BlockAllocator::ComputeRequiredMemorySize(size_t block_size,
                                                 size_t num_blocks) {
  return IndexQueue::ComputeStorageSize(num_blocks) + num_blocks * block_size;
}

// static
size_t BlockAllocator::ComputeMaximumCapacity(const size_t region_size,
                                              const size_t block_size) {
  const size_t fixed_queue_size = IndexQueue::GetFixedStorageSize();
  const size_t total_size_per_element =
      IndexQueue::GetPerElementStorageSize() + block_size;
  return (region_size - fixed_queue_size) / total_size_per_element;
}

void* BlockAllocator::Alloc() {
  uint32_t index;
  if (!free_indices_.Pop(index)) {
    return nullptr;
  }
  return static_cast<void*>(first_block_ + block_size_ * index);
}

bool BlockAllocator::Free(void* memory) {
  const off_t offset = static_cast<uint8_t*>(memory) - first_block_;
  const uint32_t index = static_cast<uint32_t>(offset / block_size_);
  ABSL_ASSERT(index < num_blocks_);
  return free_indices_.Push(index);
}

}  // namespace mem
}  // namespace ipcz
