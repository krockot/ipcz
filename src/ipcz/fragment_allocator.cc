// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/fragment_allocator.h"

#include <memory>

#include "ipcz/block_allocator_pool.h"

namespace ipcz {

FragmentAllocator::FragmentAllocator() = default;

FragmentAllocator::~FragmentAllocator() = default;

void FragmentAllocator::AddBlockAllocator(uint32_t block_size,
                                          BufferId buffer_id,
                                          absl::Span<uint8_t> memory,
                                          const BlockAllocator& allocator) {
  auto [it, ok] = block_allocator_pools_.try_emplace(block_size, nullptr);
  auto& pool = it->second;
  if (ok) {
    pool = std::make_unique<BlockAllocatorPool>(block_size);
  }

  pool->AddBlockAllocator(buffer_id, memory, allocator);
}

Fragment FragmentAllocator::Allocate(uint32_t num_bytes) {
  auto it = block_allocator_pools_.find(num_bytes);
  if (it == block_allocator_pools_.end()) {
    return Fragment();
  }

  return it->second->Allocate();
}

void FragmentAllocator::Free(const Fragment& fragment) {
  auto it = block_allocator_pools_.find(fragment.size());
  if (it == block_allocator_pools_.end()) {
    return;
  }

  it->second->Free(fragment);
}

}  // namespace ipcz
