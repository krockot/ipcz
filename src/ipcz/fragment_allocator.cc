// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/fragment_allocator.h"

#include <cstddef>
#include <cstdint>
#include <memory>

#include "ipcz/block_allocator_pool.h"
#include "ipcz/buffer_id.h"
#include "ipcz/driver_memory.h"
#include "ipcz/driver_memory_mapping.h"
#include "ipcz/node_link.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {

namespace {

// FragmentAllocator may request additional shared memory capacity. This is the
// minimum size granularity of those requests. All allocations are thus a
// multiple of 64 kB.
constexpr size_t kPageGranularity = 65536;

// The minimum fragment capacity to support for new BlockAllocators. The size of
// any memory region allocated for a new BlockAllocator is the smallest multiple
// of kPageGranularity such that it can still fit at least
// kMinimumBlockAllocatorCapacity blocks.
constexpr size_t kMinimumBlockAllocatorCapacity = 8;

}  // namespace

FragmentAllocator::FragmentAllocator() = default;

FragmentAllocator::~FragmentAllocator() = default;

void FragmentAllocator::SetNodeLink(Ref<NodeLink> node_link) {
  absl::MutexLock lock(&mutex_);
  node_link_ = std::move(node_link);
}

void FragmentAllocator::AddBlockAllocator(uint32_t block_size,
                                          BufferId buffer_id,
                                          absl::Span<uint8_t> memory,
                                          const BlockAllocator& allocator) {
  absl::MutexLock lock(&mutex_);
  auto [it, ok] = block_allocator_pools_.try_emplace(block_size, nullptr);
  auto& pool = it->second;
  if (ok) {
    pool = std::make_unique<BlockAllocatorPool>(block_size);
  }

  pool->AddBlockAllocator(buffer_id, memory, allocator);
}

Fragment FragmentAllocator::Allocate(uint32_t num_bytes) {
  const uint32_t block_size = absl::bit_ceil(num_bytes);
  BlockAllocatorPool* pool;
  {
    absl::MutexLock lock(&mutex_);
    auto it = block_allocator_pools_.find(block_size);
    if (it == block_allocator_pools_.end()) {
      return Fragment();
    }

    // Note that pools in `block_allocator_pools_` live as long as `this`, so
    // it's safe to retain this pointer through the extent of Allocate().
    pool = it->second.get();
  }

  return pool->Allocate();
}

void FragmentAllocator::AllocateAsync(uint32_t num_bytes,
                                      AllocateAsyncCallback callback) {
  Fragment fragment = Allocate(num_bytes);
  if (!fragment.is_null()) {
    callback(fragment);
    return;
  }

  const uint32_t block_size = absl::bit_ceil(num_bytes);
  size_t buffer_size = block_size * kMinimumBlockAllocatorCapacity;
  if (buffer_size < kPageGranularity) {
    buffer_size = kPageGranularity;
  } else {
    buffer_size = ((buffer_size + kPageGranularity - 1) / kPageGranularity) *
                  kPageGranularity;
  }

  Ref<NodeLink> link;
  {
    absl::MutexLock lock(&mutex_);
    link = node_link_;
  }

  RequestCapacity(buffer_size, block_size,
                  [link = std::move(link), num_bytes,
                   callback = std::move(callback)](bool ok) mutable {
                    if (!ok) {
                      callback(Fragment());
                      return;
                    }

                    FragmentAllocator& self =
                        link->memory().fragment_allocator();
                    self.AllocateAsync(num_bytes, std::move(callback));
                  });
}

void FragmentAllocator::Free(const Fragment& fragment) {
  BlockAllocatorPool* pool;
  {
    absl::MutexLock lock(&mutex_);
    auto it = block_allocator_pools_.find(fragment.size());
    if (it == block_allocator_pools_.end()) {
      return;
    }
    pool = it->second.get();
  }

  pool->Free(fragment);
}

void FragmentAllocator::RequestCapacity(uint32_t buffer_size,
                                        uint32_t block_size,
                                        RequestCapacityCallback callback) {
  Ref<NodeLink> link;
  {
    absl::MutexLock lock(&mutex_);
    auto [it, need_new_request] =
        capacity_callbacks_.emplace(block_size, CapacityCallbackList());
    it->second.push_back(std::move(callback));
    if (!need_new_request) {
      return;
    }

    link = node_link_;
  }

  link->memory().AllocateBuffer(
      buffer_size, [link, block_size, callback = std::move(callback)](
                       BufferId buffer_id, DriverMemory memory,
                       DriverMemoryMapping& mapping) {
        FragmentAllocator& self = link->memory().fragment_allocator();
        CapacityCallbackList callbacks;
        {
          absl::MutexLock lock(&self.mutex_);
          auto it = self.capacity_callbacks_.find(block_size);
          if (it != self.capacity_callbacks_.end()) {
            callbacks = std::move(it->second);
            self.capacity_callbacks_.erase(it);
          }
        }

        const bool succeeded = memory.is_valid();
        if (succeeded) {
          BlockAllocator block_allocator(mapping.bytes(), block_size);
          block_allocator.InitializeRegion();
          self.AddBlockAllocator(block_size, buffer_id, mapping.bytes(),
                                 block_allocator);

          link->AddFragmentAllocatorBuffer(buffer_id, block_size,
                                           std::move(memory));
        }

        for (auto& capacity_callback : callbacks) {
          capacity_callback(succeeded);
        }
      });
}
}  // namespace ipcz
