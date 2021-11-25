// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mem/mpsc_queue.h"

#include <memory>
#include <thread>

#include "os/memory.h"
#include "test/test_client.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/synchronization/notification.h"

namespace ipcz {
namespace mem {
namespace {

using MpscQueueTest = testing::Test;

constexpr size_t kNumProducers = 8;
constexpr size_t kNumElementsPerProducer = 10000;
constexpr size_t kNumElementsTotal = kNumProducers * kNumElementsPerProducer;
constexpr size_t kQueueLength = 16;

using TestQueue = MpscQueue<size_t, kQueueLength>;

class MpscQueueClient;

TEST_F(MpscQueueTest, Basic) {
  test::TestClient client("MpscQueueClient");
  os::Memory memory(sizeof(TestQueue));
  os::Memory::Mapping mapping = memory.Map();
  TestQueue& queue = *mapping.As<TestQueue>();

  os::Handle handle = memory.TakeHandle();
  client.channel().Send({{"hey"}, {&handle, 1}});

  std::vector<size_t> elements(kNumElementsTotal);
  for (size_t i = 0; i < kNumElementsTotal; ++i) {
    while (!queue.PopFront(elements[i]))
      ;
  }

  // Verify that we popped exactly the right number of unique values, i.e.
  // exactly one of each number in the range [0, kNumElementsTotal). Also check
  // that elements from any one producer are popped in the same order they were
  // pushed.
  std::vector<bool> popped(kNumElementsTotal);
  size_t num_unique_values_received = 0;
  for (size_t i = 0; i < kNumElementsTotal; ++i) {
    size_t value = elements[i];
    if (!popped[value]) {
      ++num_unique_values_received;
      popped[value] = true;
    }
    if (value >= kNumProducers) {
      // If values from a single producer pop in the order they were pushed, it
      // must be true that the previous value sent by this producer (which is
      // just the value `kNumProducers` elements behind us) was already popped.
      EXPECT_TRUE(popped[value - kNumProducers]);
    }
  }
  EXPECT_EQ(kNumElementsTotal, num_unique_values_received);
}

TEST_CLIENT(MpscQueueClient, c) {
  absl::Notification ready;
  os::Memory::Mapping mapping;
  c.Listen([&ready, &mapping](os::Channel::Message message) {
    ABSL_ASSERT(message.handles.size() == 1u);
    os::Memory memory(std::move(message.handles[0]), sizeof(TestQueue));
    mapping = memory.Map();
    ready.Notify();
    return true;
  });
  ready.WaitForNotification();
  TestQueue& queue = *mapping.As<TestQueue>();

  std::unique_ptr<std::thread> producers[kNumProducers];
  for (size_t i = 0; i < kNumProducers; ++i) {
    producers[i] = std::make_unique<std::thread>([id = i, &queue] {
      for (size_t i = 0; i < kNumElementsPerProducer; ++i) {
        while (!queue.PushBack(i * kNumProducers + id))
          ;
      }
    });
  }

  for (auto& producer : producers) {
    producer->join();
  }
}

}  // namespace
}  // namespace mem
}  // namespace ipcz
