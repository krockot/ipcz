// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mem/spsc_queue.h"

#include <cstddef>
#include <thread>

#include "os/memory.h"
#include "test/test_client.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/synchronization/notification.h"

namespace ipcz {
namespace mem {
namespace {

using SpscQueueTest = testing::Test;

constexpr size_t kQueueLength = 1000;

using TestQueue = SpscQueue<size_t, kQueueLength>;

class SpscQueueClient;

TEST_F(SpscQueueTest, Basic) {
  os::Memory memory1(sizeof(TestQueue));
  os::Memory memory2(sizeof(TestQueue));
  os::Memory::Mapping mapping1 = memory1.Map();
  os::Memory::Mapping mapping2 = memory2.Map();
  TestQueue& queue1 = *mapping1.As<TestQueue>();
  TestQueue& queue2 = *mapping2.As<TestQueue>();

  os::Handle handles[] = {memory1.TakeHandle(), memory2.TakeHandle()};
  test::TestClient client("SpscQueueClient");
  client.channel().Send({{"hi"}, handles});

  std::thread producer_thread([&] {
    for (size_t i = 0; i < kQueueLength; ++i) {
      queue1.PushBack(i);
    }
  });
  for (size_t i = 0; i < kQueueLength; ++i) {
    size_t x = 0;
    while (!queue2.PopFront(x))
      ;
    EXPECT_EQ(x, kQueueLength - i);
  }
  producer_thread.join();
}

TEST_CLIENT(SpscQueueClient, c) {
  absl::Notification ready;
  os::Memory::Mapping mapping1;
  os::Memory::Mapping mapping2;
  c.Listen([&ready, &mapping1, &mapping2](os::Channel::Message message) {
    ABSL_ASSERT(message.handles.size() == 2);
    os::Memory memory1(std::move(message.handles[0]), sizeof(TestQueue));
    os::Memory memory2(std::move(message.handles[1]), sizeof(TestQueue));
    mapping1 = memory1.Map();
    mapping2 = memory2.Map();
    ready.Notify();
    return true;
  });
  ready.WaitForNotification();
  TestQueue& queue1 = *mapping1.As<TestQueue>();
  TestQueue& queue2 = *mapping2.As<TestQueue>();

  for (size_t i = 0; i < kQueueLength; ++i) {
    size_t x = 0;
    while (!queue1.PopFront(x))
      ;
    queue2.PushBack(kQueueLength - x);
  }
}

}  // namespace
}  // namespace mem
}  // namespace ipcz
