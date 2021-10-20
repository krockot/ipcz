// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "os/channel.h"

#include <tuple>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/synchronization/notification.h"

namespace ipcz {
namespace os {
namespace {

using ChannelTest = testing::Test;

const char kTestMessage1[] = "Hello, world!";
const char kTestMessage2[] = "Goodbye, world!";

TEST_F(ChannelTest, ReadWriteWithHandles) {
  Channel a, b;
  std::tie(a, b) = Channel::CreateChannelPair();

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

  Channel c, d;
  std::tie(c, d) = Channel::CreateChannelPair();

  Handle ch = c.TakeHandle();
  a.Send({Channel::Data(kTestMessage1), {&ch, 1}});
  a.Reset();

  d.Send({{kTestMessage2}});
  d.Reset();

  b_finished.WaitForNotification();
}

}  // namespace
}  // namespace os
}  // namespace ipcz
