// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/block_allocator_pool.h"

#include <atomic>

#include "debug/log.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {
namespace core {

BlockAllocatorPool::Entry::Entry(BufferId buffer_id,
                                 absl::Span<uint8_t> buffer_memory,
                                 const mem::BlockAllocator& allocator)
    : buffer_id(buffer_id),
      buffer_memory(buffer_memory),
      allocator(allocator) {}

BlockAllocatorPool::Entry::~Entry() = default;

BlockAllocatorPool::BlockAllocatorPool(uint32_t block_size)
    : block_size_(block_size) {}

BlockAllocatorPool::~BlockAllocatorPool() = default;

void BlockAllocatorPool::AddAllocator(BufferId buffer_id,
                                      absl::Span<uint8_t> buffer_memory,
                                      const mem::BlockAllocator& allocator) {
  absl::MutexLock lock(&mutex_);
  Entry* previous_tail = nullptr;
  Entry* new_entry;
  if (!entries_.empty()) {
    previous_tail = &entries_.back();
  }

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
    void* block = entry->allocator.Alloc();
    if (block) {
      const uint64_t buffer_offset =
          (static_cast<uint8_t*>(block) - entry->buffer_memory.data());
      if (entry->buffer_memory.size() - block_size_ < buffer_offset) {
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

      return Fragment(FragmentDescriptor(entry->buffer_id, buffer_offset),
                      block);
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

  entry->allocator.Free(fragment.address());
}

}  // namespace core
}  // namespace ipcz
