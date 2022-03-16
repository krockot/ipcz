// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_BLOCK_ALLOCATOR_POOL_H_
#define IPCZ_SRC_IPCZ_BLOCK_ALLOCATOR_POOL_H_

#include <atomic>
#include <cstdint>
#include <list>

#include "ipcz/buffer_id.h"
#include "ipcz/fragment.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/block_allocator.h"

namespace ipcz {

// Manages access to a collection of BlockAllocators for the same block size,
// encapsulating the decision of which allocator to use for each allocation
// request. A FragmentAllocator uses BlockAllocatorPools to facilitate dynamic
// allocation of most smallish fragments.
class BlockAllocatorPool {
 public:
  explicit BlockAllocatorPool(uint32_t fragment_size);
  ~BlockAllocatorPool();

  // Permanently registers a new BlockAllocator with this object, utilizing
  // `memory` for its storage. `buffer_id` is the BufferId associated with the
  // allocator's memory and `buffer_memory` is the full span of bytes mapped by
  // the buffer. `allocator` is an BufferAllocator already initialized over some
  // subset of `buffer_memory`.
  void AddBlockAllocator(BufferId buffer_id,
                         absl::Span<uint8_t> buffer_memory,
                         const BlockAllocator& allocator);

  // Allocates a new fragment. If allocation fails because there is no capacity
  // left in any of the this object's BlockAllocators, this returns null value.
  Fragment Allocate();

  // Releases a fragment back to the allocator.
  void Free(const Fragment& fragment);

 private:
  struct Entry {
    Entry(BufferId buffer_id,
          absl::Span<uint8_t> buffer_memory,
          const BlockAllocator& allocator);
    ~Entry();

    BufferId buffer_id;
    absl::Span<uint8_t> buffer_memory;
    BlockAllocator block_allocator;
    Entry* next = nullptr;
  };

  const uint32_t fragment_size_;

  absl::Mutex mutex_;
  std::list<Entry> entries_ ABSL_GUARDED_BY(mutex_);
  absl::flat_hash_map<BufferId, Entry*> entry_map_ ABSL_GUARDED_BY(mutex_);

  // Atomic pointer to the Entry most recently used for successful allocation.
  // This generally only changes when allocation fails and a new allocator must
  // selected.
  std::atomic<Entry*> active_entry_{nullptr};
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_BLOCK_ALLOCATOR_POOL_H_
