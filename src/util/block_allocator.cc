// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/block_allocator.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>

#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

#if defined(THREAD_SANITIZER)
#include <sanitizer/tsan_interface.h>
#endif

namespace ipcz {

namespace {

// Structure stored at the start of each block when the block is not in use.
struct IPCZ_ALIGN(4) BlockHeader {
  uint16_t version;
  uint16_t next;
};

static_assert(sizeof(BlockHeader) == 4, "Invalid BlockHeader size");

BlockHeader MakeHeader(uint32_t version, uint32_t next) {
  return {static_cast<uint16_t>(version), static_cast<uint16_t>(next)};
}

}  // namespace

struct IPCZ_ALIGN(8) BlockAllocator::Block {
  std::atomic<BlockHeader> header;
};

BlockAllocator::BlockAllocator() = default;

BlockAllocator::BlockAllocator(absl::Span<uint8_t> region, uint32_t block_size)
    : region_(region),
      block_size_(block_size),
      num_blocks_(static_cast<uint32_t>(region.size()) / block_size) {
  ABSL_ASSERT(block_size >= 8);
  ABSL_ASSERT(num_blocks_ > 0);

  // BlockHeader uses a 16-bit index to reference other blocks.
  ABSL_ASSERT(num_blocks_ <= 65536);

  // Require 8-byte alignment of the region and of block sizes, to ensure that
  // each BlockHeader is itself 8-byte aligned.
  ABSL_ASSERT((reinterpret_cast<uintptr_t>(region_.data()) & 7) == 0);
  ABSL_ASSERT((block_size & 7) == 0);
}

BlockAllocator::BlockAllocator(const BlockAllocator&) = default;

BlockAllocator& BlockAllocator::operator=(const BlockAllocator&) = default;

BlockAllocator::~BlockAllocator() = default;

void BlockAllocator::InitializeRegion() {
  memset(region_.data(), 0, region_.size());
  for (uint32_t i = 0; i < num_blocks_; ++i) {
    block_at(i).header.store(MakeHeader(0, i + 1), std::memory_order_relaxed);
  }
}

void* BlockAllocator::Alloc() {
  Block& front = block_at(0);
  BlockHeader front_header = front.header.load(std::memory_order_relaxed);
  for (;;) {
    if (front_header.next == 0 || front_header.next >= num_blocks_) {
      return nullptr;
    }

    Block& candidate = block_at(front_header.next);

#if defined(THREAD_SANITIZER)
    // Subtle: Annotation here to silence TSAN. We don't really need an acquire
    // on the candidate header load below, because its result will only be used
    // if the subsequent exchange succeeds. That can only happen in practice if
    // we *didn't* race with some other thread to allocate a block, and that
    // implies that the loaded header data is still the same data that was
    // written when the block was initialized or last freed.
    __tsan_acquire(&candidate.header);
#endif

    BlockHeader candidate_header =
        candidate.header.load(std::memory_order_relaxed);
    if (!front.header.compare_exchange_weak(
            front_header,
            MakeHeader(front_header.version + 1, candidate_header.next),
            std::memory_order_release, std::memory_order_relaxed)) {
      continue;
    }

    return &candidate;
  }
}

bool BlockAllocator::Free(void* ptr) {
  Block& free_block = *static_cast<Block*>(ptr);
  const uint32_t free_index = index_of(free_block);
  if (free_index == 0 || free_index >= num_blocks_) {
    return false;
  }

  BlockHeader front_header;
  Block& front = block_at(0);
  do {
    front_header = front.header.load(std::memory_order_acquire);
    free_block.header.store(front_header, std::memory_order_relaxed);
  } while (!front.header.compare_exchange_weak(
      front_header, MakeHeader(front_header.version + 1, free_index),
      std::memory_order_release, std::memory_order_relaxed));
  return true;
}

}  // namespace ipcz
