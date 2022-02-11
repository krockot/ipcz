// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_MEM_BLOCK_ALLOCATOR_H_
#define IPCZ_SRC_MEM_BLOCK_ALLOCATOR_H_

#include <cstddef>
#include <cstdint>

#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace mem {

// BlockAllocator manages a region of memory, breaking it into dynamically
// allocable blocks of a smaller fixed size. The most recently freed block is
// always the next to be allocated.
//
// This is a thread-safe, lock-free implementation which doesn't store heap
// pointers within the managed region. Multiple BlockAllocators may therefore
// manage the same region of memory for the same block size, across different
// threads or processes.
class BlockAllocator {
 public:
  BlockAllocator();

  // Constructs a BlockAllocator to manage the memory within `region`,
  // allocating blocks of `block_size` bytes. Note that this DOES NOT initialize
  // the region. Before any BlockAllocators can allocate blocks from `region`,
  // InitializeRegion() must be called once by a single allocator managing that
  // region.
  BlockAllocator(absl::Span<uint8_t> region, size_t block_size);

  BlockAllocator(const BlockAllocator&);
  BlockAllocator& operator=(const BlockAllocator&);
  ~BlockAllocator();

  size_t block_size() const { return block_size_; }

  size_t capacity() const {
    // Note that the first block cannot be allocated, so real capacity is one
    // short.
    ABSL_ASSERT(num_blocks_ > 0);
    return num_blocks_ - 1;
  }

  // Performs a one-time initialization of the memory region managed by this
  // allocator. Many allocators may operate on the same region, but only one
  // must initialize that region, and it must do so before any of them can
  // allocate blocks.
  void InitializeRegion();

  // Allocates a block from the allocator's managed region of memory and returns
  // a pointer to its base address, where `block_size()` contiguous bytes are
  // then owned by the caller. May return null if out of blocks.
  void* Alloc();

  // Frees a block back to the allocator, given its base address as returned by
  // a prior call to Alloc(). Returns true on success, or false on failure.
  // Failure implies that `ptr` was not a valid block to free.
  bool Free(void* ptr);

 private:
  struct Block;

  Block& block(size_t index) {
    return *reinterpret_cast<Block*>(&region_[block_size_ * index]);
  }

  Block& front_block() { return block(0); }

  uint32_t index(Block& block) {
    return (reinterpret_cast<uint8_t*>(&block) - region_.data()) / block_size_;
  }

  absl::Span<uint8_t> region_;
  size_t block_size_ = 0;
  size_t num_blocks_ = 0;
};

}  // namespace mem
}  // namespace ipcz

#endif  // IPCZ_SRC_MEM_BLOCK_ALLOCATOR_H_
