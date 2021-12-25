// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mem/mpmc_queue.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "os/memory.h"
#include "test/test_client.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/synchronization/notification.h"

namespace ipcz {
namespace mem {
namespace {

using MpmcQueueTest = testing::Test;

constexpr size_t kNumProducers = 8;
constexpr size_t kNumElementsPerProducer = 5000;
constexpr size_t kNumElementsTotal = kNumProducers * kNumElementsPerProducer;
constexpr size_t kNumConsumers = kNumProducers;
constexpr size_t kNumElementsPerConsumer = kNumElementsPerProducer;
constexpr size_t kQueueLength = 15;

using TestQueue = MpmcQueue<size_t>;

class MpmcQueueClient;

TEST_F(MpmcQueueTest, Basic) {
  test::TestClient client("MpmcQueueClient");
  os::Memory memory(TestQueue::ComputeStorageSize(kQueueLength));
  os::Memory::Mapping mapping = memory.Map();
  TestQueue queue(mapping.base(), kQueueLength, TestQueue::kInitializeData);
  os::Handle handle = memory.TakeHandle();
  client.channel().Send({{"yo"}, {&handle, 1}});

  std::vector<size_t> elements(kNumElementsTotal);
  std::vector<std::unique_ptr<std::thread>> consumers(kNumConsumers);
  for (size_t i = 0; i < kNumConsumers; ++i) {
    consumers[i] = std::make_unique<std::thread>([id = i, &queue, &elements] {
      for (size_t i = 0; i < kNumElementsPerConsumer; ++i) {
        const size_t index = i * kNumConsumers + id;
        while (!queue.Pop(elements[index]))
          ;
      }
    });
  }

  // Join all the consumer threads to ensure the queue is drained before
  // validating our results.
  for (auto& consumer : consumers) {
    consumer->join();
  }
  consumers.clear();

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

TEST_CLIENT(MpmcQueueClient, c) {
  os::Memory::Mapping mapping;
  absl::Notification ready;
  c.Listen([&ready, &mapping](os::Channel::Message message) {
    ABSL_ASSERT(message.handles.size() == 1u);
    os::Memory memory(std::move(message.handles[0]), sizeof(TestQueue));
    mapping = memory.Map();
    ready.Notify();
    return true;
  });
  ready.WaitForNotification();
  c.StopListening();
  TestQueue queue(mapping.base(), kQueueLength, TestQueue::kAlreadyInitialized);

  std::unique_ptr<std::thread> producers[kNumProducers];
  for (size_t i = 0; i < kNumProducers; ++i) {
    producers[i] = std::make_unique<std::thread>([id = i, &queue] {
      for (size_t i = 0; i < kNumElementsPerProducer; ++i) {
        while (!queue.Push(i * kNumProducers + id))
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
