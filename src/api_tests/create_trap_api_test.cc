// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using CreateTrapAPITest = test::APITest;

TEST_F(CreateTrapAPITest, InvalidArgs) {
  IpczHandle trap;
  auto handler = [](const IpczTrapEvent* event) {};

  // Null conditions.
  EXPECT_EQ(
      IPCZ_RESULT_INVALID_ARGUMENT,
      ipcz.CreateTrap(q, nullptr, handler, 0, IPCZ_NO_FLAGS, nullptr, &trap));

  // Invalid conditions.
  IpczTrapConditions conditions = {0};
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.CreateTrap(q, &conditions, handler, 0, IPCZ_NO_FLAGS, nullptr,
                            &trap));

  // Invalid portal.
  conditions.size = sizeof(conditions);
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.CreateTrap(IPCZ_INVALID_HANDLE, &conditions, handler, 0,
                            IPCZ_NO_FLAGS, nullptr, &trap));

  // Null handler.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.CreateTrap(q, &conditions, nullptr, 0, IPCZ_NO_FLAGS, nullptr,
                            &trap));

  // Null output handle.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.CreateTrap(q, &conditions, handler, 0, IPCZ_NO_FLAGS, nullptr,
                            nullptr));
}

}  // namespace
}  // namespace ipcz
