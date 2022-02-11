// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mem/block_allocator.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>

#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace mem {

namespace {

enum Status : uint8_t {
  kFree = 0,
  kBusy = 1,
};

struct IPCZ_ALIGN(4) BlockHeader {
  Status status : 1;
  uint32_t next : 31;
};

}  // namespace

struct IPCZ_ALIGN(8) BlockAllocator::Block {
  std::atomic<BlockHeader> header;
  uint32_t padding;

  // The first byte of data in this block. The rest of the data follows
  // contiguously.
  uint8_t data;

  static constexpr size_t overhead() {
    return offsetof(Block, data);
  }

  static Block& ForData(void* ptr) {
    return *reinterpret_cast<Block*>(static_cast<uint8_t*>(ptr) - overhead());
  }

  static size_t GetMaxNumBlocks(size_t region_size, size_t block_size) {
    return region_size / (block_size + overhead());
  }
};

BlockAllocator::BlockAllocator() = default;

BlockAllocator::BlockAllocator(absl::Span<uint8_t> region, size_t block_size)
    : region_(region),
      block_size_(block_size),
      num_blocks_(region.size() / (block_size + Block::overhead())) {
  ABSL_ASSERT(block_size >= 1);
  ABSL_ASSERT(num_blocks_ > 0);
}

BlockAllocator::BlockAllocator(const BlockAllocator&) = default;

BlockAllocator& BlockAllocator::operator=(const BlockAllocator&) = default;

BlockAllocator::~BlockAllocator() = default;

void BlockAllocator::InitializeRegion() {
  memset(region_.data(), 0, region_.size());
  for (size_t i = 0; i < num_blocks_; ++i) {
    block(i).header.store({kFree, static_cast<uint32_t>(i + 1)},
                          std::memory_order_relaxed);
  }
}

void* BlockAllocator::Alloc() {
  BlockHeader front_header = {kFree, 1};
  for (;;) {
    if (front_header.next == 0 || front_header.next >= num_blocks_) {
      return nullptr;
    }

    Block& front = front_block();
    if (!front.header.compare_exchange_weak(
            front_header, {kBusy, front_header.next}, std::memory_order_acquire,
            std::memory_order_relaxed)) {
      continue;
    }

    Block& candidate = block(front_header.next);
    BlockHeader candidate_header =
        candidate.header.load(std::memory_order_relaxed);
    front_header.status = kBusy;
    if (!front.header.compare_exchange_strong(
            front_header, {kFree, candidate_header.next},
            std::memory_order_release, std::memory_order_relaxed)) {
      return nullptr;
    }

    return &candidate.data;
  }
}

bool BlockAllocator::Free(void* ptr) {
  Block& free_block = Block::ForData(ptr);
  const uint32_t free_index = index(free_block);
  if (free_index == 0 || free_index >= num_blocks_) {
    return false;
  }

  BlockHeader front_header;
  do {
    front_header = front_block().header.load(std::memory_order_acquire);
    free_block.header.store(front_header, std::memory_order_relaxed);
    front_header.status = kFree;
  } while (!front_block().header.compare_exchange_weak(
      front_header, {kFree, free_index}, std::memory_order_release,
      std::memory_order_relaxed));
  return true;
}

}  // namespace mem
}  // namespace ipcz
