// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mem/block_allocator.h"

#include <set>

#include "debug/log.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace mem {
namespace {

using BlockAllocatorTest = testing::Test;

TEST_F(BlockAllocatorTest, Basic) {
  uint8_t page[4096];
  constexpr size_t kBlockSize = 32;
  mem::BlockAllocator allocator(absl::MakeSpan(page), kBlockSize);
  allocator.InitializeRegion();
  std::set<void*> blocks;
  for (size_t i = 0; i < allocator.capacity(); ++i) {
    void* block = allocator.Alloc();
    EXPECT_TRUE(block);
    auto result = blocks.insert(block);
    EXPECT_TRUE(result.second);
  }
  EXPECT_FALSE(allocator.Alloc());
  for (void* block : blocks) {
    const bool result = allocator.Free(block);
    EXPECT_TRUE(result);
  }
  EXPECT_TRUE(allocator.Alloc());
}

}  // namespace
}  // namespace mem
}  // namespace ipcz
