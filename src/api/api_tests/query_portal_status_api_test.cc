// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using QueryPortalStatusAPITest = test::APITest;

TEST_F(QueryPortalStatusAPITest, InvalidArgs) {
  IpczHandle a, b;
  ipcz.OpenPortals(node(), IPCZ_NO_FLAGS, nullptr, &a, &b);

  // Null status
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.QueryPortalStatus(a, IPCZ_NO_FLAGS, nullptr, nullptr));

  // Invalid status size.
  IpczPortalStatus status = {0};
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.QueryPortalStatus(a, IPCZ_NO_FLAGS, nullptr, &status));

  // Invalid portal handle.
  status.size = sizeof(status);
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.QueryPortalStatus(IPCZ_INVALID_HANDLE, IPCZ_NO_FLAGS, nullptr,
                                   &status));

  ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr);
  ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr);
}

TEST_F(QueryPortalStatusAPITest, ClosedBit) {
  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node(), IPCZ_NO_FLAGS, nullptr, &a, &b));

  IpczPortalStatus status = {sizeof(status)};
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.QueryPortalStatus(a, IPCZ_NO_FLAGS, nullptr, &status));
  EXPECT_EQ(IPCZ_NO_FLAGS, status.flags);
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.QueryPortalStatus(a, IPCZ_NO_FLAGS, nullptr, &status));
  EXPECT_EQ(IPCZ_PORTAL_STATUS_PEER_CLOSED | IPCZ_PORTAL_STATUS_DEAD,
            status.flags);

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr));
}

}  // namespace
}  // namespace ipcz
