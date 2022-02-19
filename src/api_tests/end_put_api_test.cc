// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using EndPutAPITest = test::APITest;

TEST_F(EndPutAPITest, InvalidArgs) {
  // Invalid handle.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.EndPut(IPCZ_INVALID_HANDLE, 0, nullptr, 0, nullptr, 0,
                        IPCZ_NO_FLAGS, nullptr));

  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node, IPCZ_NO_FLAGS, nullptr, &a, &b));

  // Null IpczHandle buffer with non-zero handle count.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.EndPut(a, 0, nullptr, 1, nullptr, 0, IPCZ_NO_FLAGS, nullptr));

  // Null OS handle buffer with non-zero handle count.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.EndPut(a, 0, nullptr, 0, nullptr, 1, IPCZ_NO_FLAGS, nullptr));

  // Out of bounds from original BeginPut()
  uint32_t num_bytes = 4;
  void* data;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.BeginPut(a, IPCZ_NO_FLAGS, nullptr, &num_bytes, &data));
  EXPECT_EQ(4u, num_bytes);
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.EndPut(a, num_bytes * 2, nullptr, 0, nullptr, 0, IPCZ_NO_FLAGS,
                        nullptr));

  ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr);
  ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr);
}

TEST_F(EndPutAPITest, NoPutInProgress) {
  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node, IPCZ_NO_FLAGS, nullptr, &a, &b));

  EXPECT_EQ(IPCZ_RESULT_FAILED_PRECONDITION,
            ipcz.EndPut(a, 4, nullptr, 0, nullptr, 0, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(
      IPCZ_RESULT_FAILED_PRECONDITION,
      ipcz.EndPut(a, 4, nullptr, 0, nullptr, 0, IPCZ_END_PUT_ABORT, nullptr));

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr));
}

TEST_F(EndPutAPITest, Oversized) {
  IpczHandle a, b;
  ipcz.OpenPortals(node, IPCZ_NO_FLAGS, nullptr, &a, &b);
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.BeginPut(a, IPCZ_NO_FLAGS, nullptr, nullptr, nullptr));
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.EndPut(a, 4, nullptr, 0, nullptr, 0, IPCZ_NO_FLAGS, nullptr));
  ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr);
  ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr);
}

}  // namespace
}  // namespace ipcz
