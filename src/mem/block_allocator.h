// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_MEM_BLOCK_ALLOCATOR_H_
#define IPCZ_SRC_MEM_BLOCK_ALLOCATOR_H_

#include <cstddef>
#include <cstdint>

#include "mem/mpmc_queue.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace mem {

// BlockAllocator exposes a pool of memory as fixed number of dynamically
// allocatable blocks of fixed size. It contains no internal heap references and
// is safe to host within a shared memory region.
//
// The pool of memory is divided into two sections: a series of N fixed-size,
// contiguous blocks within the pool, and a preceding MpmcQueue of capacity N,
// which is used as a free-list of block indices.
//
// Allocating a block pops the next available index off the queue, and freeing a
// block pushes its index back onto the queue. The queue is initialized as full,
// containing each index from 0 to N-1.
class BlockAllocator {
 public:
  enum { kInitialize };
  enum { kAlreadyInitialized };

  BlockAllocator(void* memory,
                 const size_t block_size,
                 const size_t num_blocks,
                 decltype(kAlreadyInitialized));
  BlockAllocator(void* memory,
                 const size_t block_size,
                 const size_t num_blocks,
                 decltype(kInitialize));
  BlockAllocator(absl::Span<uint8_t> region,
                 size_t block_size,
                 decltype(kAlreadyInitialized));
  BlockAllocator(absl::Span<uint8_t> region,
                 size_t block_size,
                 decltype(kInitialize));
  ~BlockAllocator();

  size_t block_size() const { return block_size_; }
  size_t num_blocks() const { return num_blocks_; }

  void* first_block() const { return first_block_; }
  void* end() const { return first_block_ + num_blocks_ * block_size_; }

  static constexpr size_t ComputeRequiredMemorySize(size_t block_size,
                                                    size_t num_blocks) {
    return IndexQueue::ComputeStorageSize(num_blocks) + num_blocks * block_size;
  }

  static constexpr size_t ComputeMaximumCapacity(const size_t region_size,
                                                 const size_t block_size) {
    const size_t fixed_queue_size = IndexQueue::GetFixedStorageSize();
    const size_t total_size_per_element =
        IndexQueue::GetPerElementStorageSize() + block_size;
    return (region_size - fixed_queue_size) / total_size_per_element;
  }

  void* Alloc();
  bool Free(void* memory);

  template <typename T>
  T* AllocAs() {
    ABSL_ASSERT(sizeof(T) <= block_size_);
    return static_cast<T*>(Alloc());
  }

 private:
  using IndexQueue = MpmcQueue<uint32_t>;

  size_t queue_size() const {
    return IndexQueue::ComputeStorageSize(num_blocks_);
  }

  const size_t block_size_;
  const size_t num_blocks_;
  IndexQueue free_indices_;
  uint8_t* const first_block_;
};

}  // namespace mem
}  // namespace ipcz

#endif  // IPCZ_SRC_MEM_BLOCK_ALLOCATOR_H_
