// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using ArmTrapAPITest = test::APITest;

TEST_F(ArmTrapAPITest, InvalidArgs) {
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.ArmTrap(IPCZ_INVALID_HANDLE, 0, nullptr, nullptr, nullptr));
}

}  // namespace
}  // namespace ipcz
