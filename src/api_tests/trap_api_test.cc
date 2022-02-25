// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using TrapAPITest = test::APITest;

TEST_F(TrapAPITest, InvalidArgs) {
  auto handler = [](const IpczTrapEvent* event) {};

  // Null conditions.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Trap(q, nullptr, handler, 0, IPCZ_NO_FLAGS, nullptr, nullptr,
                      nullptr));

  // Invalid conditions.
  IpczTrapConditions conditions = {0};
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Trap(q, &conditions, handler, 0, IPCZ_NO_FLAGS, nullptr,
                      nullptr, nullptr));

  // Invalid portal.
  conditions.size = sizeof(conditions);
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Trap(IPCZ_INVALID_HANDLE, &conditions, handler, 0,
                      IPCZ_NO_FLAGS, nullptr, nullptr, nullptr));

  // Null handler.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Trap(q, &conditions, nullptr, 0, IPCZ_NO_FLAGS, nullptr,
                      nullptr, nullptr));

  // Invalid output status.
  IpczPortalStatus status = {0};
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Trap(q, &conditions, handler, 0, IPCZ_NO_FLAGS, nullptr,
                      nullptr, &status));
}

}  // namespace
}  // namespace ipcz
