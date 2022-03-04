// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "reference_drivers/event.h"

#include <thread>

#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz::reference_drivers {
namespace {

using EventTest = testing::Test;

TEST_F(EventTest, Basic) {
  Event e;
  Event::Notifier n = e.MakeNotifier();
  n.Notify();
  e.Wait();
}

TEST_F(EventTest, Threaded) {
  Event e;
  Event::Notifier n = e.MakeNotifier();
  bool signaled = false;
  std::thread t([&signaled, &n] {
    signaled = true;
    n.Notify();
  });
  e.Wait();
  EXPECT_EQ(true, signaled);
  t.join();
}

TEST_F(EventTest, MultipleNotifiers) {
  Event e;
  Event::Notifier n1 = e.MakeNotifier();
  Event::Notifier n2 = e.MakeNotifier();
  Event::Notifier n3 = n2.Clone();
  n1.Notify();
  n2.Notify();
  n3.Notify();
  e.Wait();
}

TEST_F(EventTest, SequentialWaits) {
  Event e;
  Event::Notifier n1 = e.MakeNotifier();
  Event::Notifier n2 = e.MakeNotifier();

  n1.Notify();
  n2.Notify();
  e.Wait();

  bool signaled = false;
  std::thread t([&signaled, &n1] {
    signaled = true;
    n1.Notify();
  });

  e.Wait();
  EXPECT_EQ(true, signaled);
  t.join();
}

}  // namespace
}  // namespace ipcz::reference_drivers
