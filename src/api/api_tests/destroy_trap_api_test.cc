// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using DestroyTrapAPITest = test::APITest;

TEST_F(DestroyTrapAPITest, InvalidArgs) {
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.DestroyTrap(p, IPCZ_INVALID_HANDLE, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.DestroyTrap(IPCZ_INVALID_HANDLE, IPCZ_INVALID_HANDLE,
                             IPCZ_NO_FLAGS, nullptr));

  IpczTrapConditions conditions = {sizeof(conditions)};
  auto handler = [](const IpczTrapEvent* event) {};
  IpczHandle trap;
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.CreateTrap(p, &conditions, handler, 0,
                                            IPCZ_NO_FLAGS, nullptr, &trap));
  ASSERT_NE(IPCZ_INVALID_HANDLE, trap);
  EXPECT_EQ(
      IPCZ_RESULT_INVALID_ARGUMENT,
      ipcz.DestroyTrap(IPCZ_INVALID_HANDLE, trap, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.DestroyTrap(p, trap, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.DestroyTrap(p, trap, IPCZ_NO_FLAGS, nullptr));
}

}  // namespace
}  // namespace ipcz
