// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/block_allocator.h"

#include <array>
#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/random.h"

namespace ipcz {
namespace {

constexpr size_t kPageSize = 4096;
constexpr size_t kBlockSize = 32;

class BlockAllocatorTest : public testing::Test {
 public:
  BlockAllocatorTest() {
    allocator = BlockAllocator(page, kBlockSize);
    allocator.InitializeRegion();
  }

 protected:
  uint8_t page[kPageSize];
  BlockAllocator allocator;
};

TEST_F(BlockAllocatorTest, Basic) {
  std::set<void*> blocks;
  for (size_t i = 0; i < allocator.capacity(); ++i) {
    void* block = allocator.Alloc();
    EXPECT_TRUE(block);
    memset(block, 0xaa, kBlockSize);
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

TEST_F(BlockAllocatorTest, StressTest) {
  // This test creates a bunch of worker threads to race Alloc() and Free()
  // operations. Workers mark their allocated blocks and verify consistency of
  // markers when freeing them. Once all workers have finished, the test
  // verifies that all blocks are still allocable (i.e. none were lost due to
  // racy accounting errors.)

  static constexpr size_t kNumIterationsPerWorker = 1000;
  static constexpr size_t kNumAllocationsPerIteration = 50;
  auto worker = [this](uint32_t id) {
    std::array<std::atomic<uint32_t>*, kNumAllocationsPerIteration> allocations;
    for (size_t i = 0; i < kNumIterationsPerWorker; ++i) {
      size_t num_allocations = 0;
      for (size_t j = 0; j < kNumAllocationsPerIteration; ++j) {
        if (auto* p = static_cast<std::atomic<uint32_t>*>(allocator.Alloc())) {
          allocations[num_allocations++] = p;
          p->store(id, std::memory_order_relaxed);
        }
      }
      for (size_t j = 0; j < num_allocations; ++j) {
        std::atomic<uint32_t>* p = allocations[j];
        ASSERT_EQ(id, p->load(std::memory_order_relaxed));
        allocator.Free(p);
      }
    }
  };

  static constexpr uint32_t kNumWorkers = 4;
  std::vector<std::thread> worker_threads;
  for (uint32_t i = 0; i < kNumWorkers; ++i) {
    worker_threads.emplace_back(worker, i);
  }

  for (auto& t : worker_threads) {
    t.join();
  }

  size_t allocable_capacity = 0;
  for (size_t i = 0; i < allocator.capacity(); ++i) {
    void* p = allocator.Alloc();
    if (p) {
      ++allocable_capacity;
    }
  }

  EXPECT_EQ(allocator.capacity(), allocable_capacity);
}

}  // namespace
}  // namespace ipcz
