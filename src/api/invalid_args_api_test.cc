// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using InvalidArgsAPITest = test::APITest;

TEST_F(InvalidArgsAPITest, CreateNode) {
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.CreateNode(IPCZ_NO_FLAGS, nullptr, nullptr));
}

TEST_F(InvalidArgsAPITest, DestroyNode) {
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.DestroyNode(IPCZ_INVALID_HANDLE, IPCZ_NO_FLAGS, nullptr));
}

TEST_F(InvalidArgsAPITest, OpenPortals) {
  IpczHandle a, b;
  EXPECT_EQ(
      IPCZ_RESULT_INVALID_ARGUMENT,
      ipcz.OpenPortals(IPCZ_INVALID_HANDLE, IPCZ_NO_FLAGS, nullptr, &a, &b));
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.OpenPortals(node(), IPCZ_NO_FLAGS, nullptr, nullptr, &b));
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.OpenPortals(node(), IPCZ_NO_FLAGS, nullptr, &a, nullptr));
}

TEST_F(InvalidArgsAPITest, ClosePortal) {
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.ClosePortal(IPCZ_INVALID_HANDLE, IPCZ_NO_FLAGS, nullptr));
}

TEST_F(InvalidArgsAPITest, QueryStatus) {
  IpczHandle a, b;
  ipcz.OpenPortals(node(), IPCZ_NO_FLAGS, nullptr, &a, &b);

  // Null status
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.QueryPortalStatus(a, IPCZ_PORTAL_STATUS_FIELD_BITS,
                                   IPCZ_NO_FLAGS, nullptr, nullptr));

  // Invalid status size.
  IpczPortalStatus status = {0};
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.QueryPortalStatus(a, IPCZ_PORTAL_STATUS_FIELD_BITS,
                                   IPCZ_NO_FLAGS, nullptr, &status));

  // Invalid portal handle.
  status.size = sizeof(status);
  EXPECT_EQ(
      IPCZ_RESULT_INVALID_ARGUMENT,
      ipcz.QueryPortalStatus(IPCZ_INVALID_HANDLE, IPCZ_PORTAL_STATUS_FIELD_BITS,
                             IPCZ_NO_FLAGS, nullptr, &status));

  ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr);
  ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr);
}

TEST_F(InvalidArgsAPITest, Put) {
  // Invalid portal
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Put(IPCZ_INVALID_HANDLE, nullptr, 0, nullptr, 0, nullptr, 0,
                     IPCZ_NO_FLAGS, nullptr));

  IpczHandle a, b;
  ipcz.OpenPortals(node(), IPCZ_NO_FLAGS, nullptr, &a, &b);

  // Invalid options
  IpczPutOptions options = {0};
  EXPECT_EQ(
      IPCZ_RESULT_INVALID_ARGUMENT,
      ipcz.Put(a, nullptr, 0, nullptr, 0, nullptr, 0, IPCZ_NO_FLAGS, &options));

  // Invalid limits
  IpczPutLimits limits = {0};
  options.size = sizeof(options);
  options.limits = &limits;
  EXPECT_EQ(
      IPCZ_RESULT_INVALID_ARGUMENT,
      ipcz.Put(a, nullptr, 0, nullptr, 0, nullptr, 0, IPCZ_NO_FLAGS, &options));

  // Null buffers with non-zero counts.
  EXPECT_EQ(
      IPCZ_RESULT_INVALID_ARGUMENT,
      ipcz.Put(a, nullptr, 1, nullptr, 0, nullptr, 0, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(
      IPCZ_RESULT_INVALID_ARGUMENT,
      ipcz.Put(a, nullptr, 0, nullptr, 1, nullptr, 0, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(
      IPCZ_RESULT_INVALID_ARGUMENT,
      ipcz.Put(a, nullptr, 0, nullptr, 0, nullptr, 1, IPCZ_NO_FLAGS, nullptr));

  // Putting a portal or its peer into itself is also invalid.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Put(a, nullptr, 0, &a, 1, nullptr, 0, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Put(a, nullptr, 0, &b, 1, nullptr, 0, IPCZ_NO_FLAGS, nullptr));

  ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr);
  ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr);
}

TEST_F(InvalidArgsAPITest, BeginPut) {
  uint32_t num_bytes = 4;
  void* data;

  // Invalid portal
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.BeginPut(IPCZ_INVALID_HANDLE, &num_bytes, IPCZ_NO_FLAGS,
                          nullptr, &data));

  IpczHandle a, b;
  ipcz.OpenPortals(node(), IPCZ_NO_FLAGS, nullptr, &a, &b);

  // Null data with non-zero data size.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.BeginPut(a, &num_bytes, IPCZ_NO_FLAGS, nullptr, nullptr));

  // Invalid options
  IpczBeginPutOptions options = {0};
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.BeginPut(a, &num_bytes, IPCZ_NO_FLAGS, &options, &data));

  // Invalid limits
  IpczPutLimits limits = {0};
  options.size = sizeof(options);
  options.limits = &limits;
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.BeginPut(a, &num_bytes, IPCZ_NO_FLAGS, &options, &data));

  ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr);
  ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr);
}

}  // namespace
}  // namespace ipcz
