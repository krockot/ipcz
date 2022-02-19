// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "reference_drivers/channel.h"

#include <tuple>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/synchronization/notification.h"
#include "util/os_handle.h"

namespace ipcz {
namespace reference_drivers {
namespace {

using ChannelTest = testing::Test;

const char kTestMessage1[] = "Hello, world!";

TEST_F(ChannelTest, ReadWrite) {
  auto [a, b] = Channel::CreateChannelPair();

  absl::Notification b_finished;
  b.Listen([&b_finished](Channel::Message message) {
    EXPECT_EQ(kTestMessage1, message.data.AsString());
    b_finished.Notify();
    return true;
  });

  a.Send({Channel::Data(kTestMessage1)});

  b_finished.WaitForNotification();
  a.Reset();
}

// Channel does not support out-of-band handle attachments on Windows, because
// Windows handle values are just part of the message data.
#if !defined(OS_WIN)
const char kTestMessage2[] = "Goodbye, world!";

TEST_F(ChannelTest, ReadWriteWithHandles) {
  auto [a, b] = Channel::CreateChannelPair();

  absl::Notification b_finished;
  b.Listen([&b_finished](Channel::Message message) {
    EXPECT_EQ(kTestMessage1, message.data.AsString());
    absl::Notification c_received_message;
    Channel c(std::move(message.handles[0]));
    absl::Notification c_finished;
    c.Listen([&c_finished](Channel::Message message) {
      EXPECT_EQ(kTestMessage2, message.data.AsString());
      c_finished.Notify();
      return true;
    });

    c_finished.WaitForNotification();
    b_finished.Notify();
    return true;
  });

  auto [c, d] = Channel::CreateChannelPair();

  OSHandle ch = c.TakeHandle();
  a.Send({Channel::Data(kTestMessage1), {&ch, 1}});
  a.Reset();

  d.Send({{kTestMessage2}});
  d.Reset();

  b_finished.WaitForNotification();
}
#endif

}  // namespace
}  // namespace reference_drivers
}  // namespace ipcz
