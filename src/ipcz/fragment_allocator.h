// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_FRAGMENT_ALLOCATOR_H_
#define IPCZ_SRC_IPCZ_FRAGMENT_ALLOCATOR_H_

#include <cstdint>
#include <memory>

#include "ipcz/buffer_id.h"
#include "ipcz/fragment.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {

class BlockAllocator;
class BlockAllocatorPool;

// Manages access to BlockAllocatorPools to facilitate dynamic shared memory
// allocation within regions shared by a NodeLinkMemory.
class FragmentAllocator {
 public:
  FragmentAllocator();
  ~FragmentAllocator();

  // Registers a new BlockAllocator for the given block size, creating a new
  // BlockAllocatorPool if necessary.
  void AddBlockAllocator(uint32_t block_size,
                         BufferId buffer_id,
                         absl::Span<uint8_t> memory,
                         const BlockAllocator& allocator);

  // Allocates a new fragment. If allocation fails because there is no capacity
  // left in any of the this object's internal allocators, this returns null
  // value.
  Fragment Allocate(uint32_t num_bytes);

  // Releases a fragment back to the allocator.
  void Free(const Fragment& fragment);

 private:
  absl::flat_hash_map<uint32_t, std::unique_ptr<BlockAllocatorPool>>
      block_allocator_pools_;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_FRAGMENT_ALLOCATOR_H_
