// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "reference_drivers/single_process_reference_driver.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using BoxAPITest = test::APITest;

TEST_F(BoxAPITest, InvalidArgs) {
  IpczHandle handle;
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Box(IPCZ_INVALID_HANDLE, 42, IPCZ_NO_FLAGS, nullptr, &handle));
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Box(node, IPCZ_INVALID_DRIVER_HANDLE, IPCZ_NO_FLAGS, nullptr,
                     &handle));
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Box(node, 42, IPCZ_NO_FLAGS, nullptr, nullptr));
}

}  // namespace
}  // namespace ipcz
