// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using MergePortalsAPITest = test::APITest;

TEST_F(MergePortalsAPITest, InvalidArgs) {
  IpczHandle a, b;
  ASSERT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node, IPCZ_NO_FLAGS, nullptr, &a, &b));
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.MergePortals(IPCZ_INVALID_HANDLE, IPCZ_INVALID_HANDLE,
                              IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.MergePortals(IPCZ_INVALID_HANDLE, b, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.MergePortals(a, IPCZ_INVALID_HANDLE, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.MergePortals(a, b, IPCZ_NO_FLAGS, nullptr));

  ipcz.Close(a, IPCZ_NO_FLAGS, nullptr);
  ipcz.Close(b, IPCZ_NO_FLAGS, nullptr);
}

}  // namespace
}  // namespace ipcz
