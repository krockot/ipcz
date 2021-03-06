// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/block_allocator_pool.h"

#include <atomic>

#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "util/log.h"

namespace ipcz {

BlockAllocatorPool::Entry::Entry(BufferId buffer_id,
                                 absl::Span<uint8_t> buffer_memory,
                                 const BlockAllocator& allocator)
    : buffer_id(buffer_id),
      buffer_memory(buffer_memory),
      block_allocator(allocator) {}

BlockAllocatorPool::Entry::~Entry() = default;

BlockAllocatorPool::BlockAllocatorPool(uint32_t fragment_size)
    : fragment_size_(fragment_size) {}

BlockAllocatorPool::~BlockAllocatorPool() = default;

size_t BlockAllocatorPool::GetCapacity() {
  absl::MutexLock lock(&mutex_);
  return capacity_;
}

void BlockAllocatorPool::AddBlockAllocator(BufferId buffer_id,
                                           absl::Span<uint8_t> buffer_memory,
                                           const BlockAllocator& allocator) {
  absl::MutexLock lock(&mutex_);
  Entry* previous_tail = nullptr;
  Entry* new_entry;
  if (!entries_.empty()) {
    previous_tail = &entries_.back();
  }

  capacity_ += allocator.capacity() * fragment_size_;
  entries_.emplace_back(buffer_id, buffer_memory, allocator);
  new_entry = &entries_.back();
  entry_map_[buffer_id] = new_entry;

  if (previous_tail) {
    previous_tail->next = new_entry;
  } else {
    active_entry_ = new_entry;
  }
}

Fragment BlockAllocatorPool::Allocate() {
  Entry* entry = active_entry_.load(std::memory_order_relaxed);
  if (!entry) {
    return {};
  }

  Entry* starting_entry = entry;
  do {
    void* block = entry->block_allocator.Alloc();
    if (block) {
      const uint32_t buffer_offset =
          (static_cast<uint8_t*>(block) - entry->buffer_memory.data());
      if (entry->buffer_memory.size() - fragment_size_ < buffer_offset) {
        // Allocator did something bad.
        DLOG(ERROR) << "Invalid address from BlockAllocator.";
        return {};
      }

      if (entry != starting_entry) {
        // Attempt to update the active entry to reflect our success. Since this
        // is only meant as a helpful hint, we don't really care if it succeeds.
        active_entry_.compare_exchange_weak(starting_entry, entry,
                                            std::memory_order_relaxed);
      }

      FragmentDescriptor descriptor(entry->buffer_id, buffer_offset,
                                    fragment_size_);
      return Fragment(descriptor, block);
    }

    // Allocation from this buffer failed. Try a different buffer.
    absl::MutexLock lock(&mutex_);
    entry = entry->next;
  } while (entry && entry != starting_entry);

  return {};
}

void BlockAllocatorPool::Free(const Fragment& fragment) {
  Entry* entry;
  {
    absl::MutexLock lock(&mutex_);
    auto it = entry_map_.find(fragment.buffer_id());
    if (it == entry_map_.end()) {
      DLOG(ERROR) << "Invalid Free() call on BlockAllocatorPool";
      return;
    }
    entry = it->second;
  }

  entry->block_allocator.Free(fragment.address());
}

}  // namespace ipcz
