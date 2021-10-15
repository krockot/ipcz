// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using GetAPITest = test::APITest;

TEST_F(GetAPITest, InvalidArgs) {
  // Invalid portal.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Get(IPCZ_INVALID_HANDLE, nullptr, nullptr, nullptr, nullptr,
                     nullptr, nullptr, IPCZ_NO_FLAGS, nullptr));

  uint32_t not_zero = 4;

  // Null data buffer but non-zero byte count
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Get(q, nullptr, &not_zero, nullptr, nullptr, nullptr, nullptr,
                     IPCZ_NO_FLAGS, nullptr));

  // Null portal buffer but non-zero portal count.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Get(q, nullptr, nullptr, nullptr, &not_zero, nullptr, nullptr,
                     IPCZ_NO_FLAGS, nullptr));

  // Null OS handle buffer but non-zero OS handle count.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Get(q, nullptr, nullptr, nullptr, nullptr, nullptr, &not_zero,
                     IPCZ_NO_FLAGS, nullptr));
}

}  // namespace
}  // namespace ipcz
