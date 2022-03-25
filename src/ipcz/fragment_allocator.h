// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_FRAGMENT_ALLOCATOR_H_
#define IPCZ_SRC_IPCZ_FRAGMENT_ALLOCATOR_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "ipcz/buffer_id.h"
#include "ipcz/fragment.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/function.h"
#include "util/ref_counted.h"

namespace ipcz {

class BlockAllocator;
class BlockAllocatorPool;
class NodeLink;

// Manages access to BlockAllocatorPools, to facilitate dynamic shared memory
// allocation within regions shared by a NodeLinkMemory.
class FragmentAllocator {
 public:
  FragmentAllocator();
  ~FragmentAllocator();

  void SetNodeLink(Ref<NodeLink> node_link);

  // Registers a new BlockAllocator for the given block size, creating a new
  // BlockAllocatorPool if necessary.
  void AddBlockAllocator(uint32_t block_size,
                         BufferId buffer_id,
                         absl::Span<uint8_t> memory,
                         const BlockAllocator& allocator);

  // Allocates a new fragment. If allocation fails because there is no capacity
  // left in any of the this object's internal allocators, this returns a null
  // fragment.
  Fragment Allocate(uint32_t num_bytes);

  // Allocates a new fragment. If allocation would fail because there is no
  // capacity in any of this object's internal allocators, this requests
  // additional capacity to be added to the NodeLinkMemory before attempting
  // allocation. When allocation is complete, `callback` is called with the
  // result.
  //
  // The callback may be called before this returns, or any time after, but it
  // will only be called once. Allocation may also fail, implying that the
  // application is OOM or has hit some other system resource limit.
  using AllocateAsyncCallback = Function<void(Fragment)>;
  void AllocateAsync(uint32_t num_bytes, AllocateAsyncCallback callback);

  // Releases a fragment back to the allocator.
  void Free(const Fragment& fragment);

 private:
  using RequestCapacityCallback = Function<void(bool)>;
  void RequestCapacity(uint32_t buffer_size,
                       uint32_t block_size,
                       RequestCapacityCallback callback);

  absl::Mutex mutex_;
  Ref<NodeLink> node_link_ ABSL_GUARDED_BY(mutex_);

  absl::flat_hash_map<uint32_t, std::unique_ptr<BlockAllocatorPool>>
      block_allocator_pools_ ABSL_GUARDED_BY(mutex_);

  // Callbacks to invoke when a pending capacity request is fulfilled for a
  // specific fragment size.
  using CapacityCallbackList = std::vector<RequestCapacityCallback>;
  absl::flat_hash_map<uint32_t, CapacityCallbackList> capacity_callbacks_
      ABSL_GUARDED_BY(mutex_);
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_FRAGMENT_ALLOCATOR_H_
