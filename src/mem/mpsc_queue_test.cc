// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mem/mpsc_queue.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/synchronization/notification.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace mem {
namespace {

using MpscQueueTest = testing::Test;

constexpr size_t kNumProducers = 4;
constexpr size_t kNumElementsPerProducer = 5000;
constexpr size_t kNumElementsTotal = kNumProducers * kNumElementsPerProducer;
constexpr size_t kQueueLength = 64;

using TestQueue = MpscQueue<size_t>;

class MpscQueueClient;

TEST_F(MpscQueueTest, Basic) {
  std::vector<uint8_t> memory(TestQueue::ComputeSpaceRequiredFor(kQueueLength));
  TestQueue queue(absl::MakeSpan(memory));
  queue.InitializeRegion();

  std::unique_ptr<std::thread> producers[kNumProducers];
  for (size_t i = 0; i < kNumProducers; ++i) {
    producers[i] = std::make_unique<std::thread>([id = i, &queue] {
      for (size_t i = 0; i < kNumElementsPerProducer; ++i) {
        while (!queue.Push(i * kNumProducers + id))
          ;
      }
    });
  }

  std::vector<size_t> elements(kNumElementsTotal);
  for (size_t& element : elements) {
    while (!queue.Pop(element))
      ;
  }

  for (auto& thread : producers) {
    thread->join();
  }

  // Verify that we popped exactly the right number of unique values, i.e.
  // exactly one of each number in the range [0, kNumElementsTotal).
  std::vector<bool> popped(kNumElementsTotal);
  size_t num_unique_values_received = 0;
  for (size_t i = 0; i < kNumElementsTotal; ++i) {
    size_t value = elements[i];
    if (!popped[value]) {
      ++num_unique_values_received;
      popped[value] = true;
    }
  }
  EXPECT_EQ(kNumElementsTotal, num_unique_values_received);
}

}  // namespace
}  // namespace mem
}  // namespace ipcz
