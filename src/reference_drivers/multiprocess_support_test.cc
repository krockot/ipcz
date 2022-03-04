// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdint>

#include "reference_drivers/channel.h"
#include "reference_drivers/event.h"
#include "reference_drivers/memory.h"
#include "reference_drivers/os_handle.h"
#include "test/test_client.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/synchronization/notification.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz::reference_drivers {
namespace {

using MultiprocessTest = testing::Test;

const char kTestMessage1[] = "Hello, world!";

TEST_F(MultiprocessTest, BasicChannel) {
  test::TestClient client("BasicClient");
  client.channel().Send({{kTestMessage1}});
}

TEST_CLIENT(BasicClient, c) {
  absl::Notification received;
  c.Listen([&received](Channel::Message message) {
    EXPECT_EQ(kTestMessage1, message.data.AsString());
    received.Notify();
    return true;
  });
  received.WaitForNotification();
}

#if !defined(OS_WIN)
TEST_F(MultiprocessTest, PassMemory) {
  test::TestClient client("PassMemoryClient");
  Memory memory(8);

  Memory::Mapping mapping = memory.Map();

  int& shared_int = mapping.As<int>()[0];
  shared_int = 42;

  OSHandle h = memory.TakeHandle();
  client.channel().Send({{"hi"}, {&h, 1}});
  EXPECT_EQ(0, client.Wait());

  EXPECT_EQ(43, shared_int);
}

TEST_CLIENT(PassMemoryClient, c) {
  OSHandle memory_handle;
  absl::Notification received;
  c.Listen([&received, &memory_handle](Channel::Message message) {
    EXPECT_EQ("hi", message.data.AsString());
    ABSL_ASSERT(message.handles.size() == 1u);
    memory_handle = std::move(message.handles[0]);
    received.Notify();
    return true;
  });
  received.WaitForNotification();
  c.StopListening();

  Memory::Mapping mapping = Memory(std::move(memory_handle), 8).Map();
  int& shared_int = mapping.As<int>()[0];

  EXPECT_EQ(42, shared_int);
  shared_int = 43;
}

TEST_F(MultiprocessTest, SynchronizedMemory) {
  test::TestClient client("SynchronizedMemoryClient");

  Memory memory(8);
  Memory::Mapping mapping = memory.Map();

  uint64_t& shared_int = mapping.As<uint64_t>()[0];
  shared_int = 42;

  Event my_event;
  Event their_event;
  Event::Notifier notify_them = their_event.MakeNotifier();
  OSHandle handles[] = {
      memory.TakeHandle(),
      my_event.MakeNotifier().TakeHandle(),
      their_event.TakeHandle(),
  };
  client.channel().Send({{"hey"}, handles});

  my_event.Wait();
  EXPECT_EQ(43u, shared_int);

  shared_int = 44;
  notify_them.Notify();

  EXPECT_EQ(0, client.Wait());
  EXPECT_EQ(45u, shared_int);
}

TEST_CLIENT(SynchronizedMemoryClient, c) {
  Memory::Mapping mapping;
  Event::Notifier notifier;
  Event event;
  absl::Notification received;
  c.Listen([&mapping, &notifier, &event, &received](Channel::Message message) {
    ABSL_ASSERT(message.handles.size() == 3u);
    mapping = Memory(std::move(message.handles[0]), 8).Map();
    notifier = Event::Notifier(std::move(message.handles[1]));
    event = Event(std::move(message.handles[2]));
    received.Notify();
    return true;
  });
  received.WaitForNotification();

  uint64_t& shared_int = mapping.As<uint64_t>()[0];
  EXPECT_EQ(42u, shared_int);

  shared_int = 43;
  notifier.Notify();
  event.Wait();

  EXPECT_EQ(44u, shared_int);
  shared_int = 45;
}
#endif

}  // namespace
}  // namespace ipcz::reference_drivers
