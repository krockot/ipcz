// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using BeginPutAPITest = test::APITest;

TEST_F(BeginPutAPITest, InvalidArgs) {
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

TEST_F(BeginPutAPITest, NoOverlap) {
  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node(), IPCZ_NO_FLAGS, nullptr, &a, &b));

  void* data;
  uint32_t num_bytes = 4;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.BeginPut(a, &num_bytes, IPCZ_NO_FLAGS, nullptr, &data));
  EXPECT_EQ(IPCZ_RESULT_ALREADY_EXISTS,
            ipcz.BeginPut(a, &num_bytes, IPCZ_NO_FLAGS, nullptr, &data));
  EXPECT_EQ(
      IPCZ_RESULT_ALREADY_EXISTS,
      ipcz.Put(a, nullptr, 0, nullptr, 0, nullptr, 0, IPCZ_NO_FLAGS, nullptr));

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr));
}

TEST_F(BeginPutAPITest, ParcelLimit) {
  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node(), IPCZ_NO_FLAGS, nullptr, &a, &b));

  IpczPutLimits limits = {sizeof(limits)};
  limits.max_queued_parcels = 2;
  IpczBeginPutOptions options = {sizeof(options)};
  options.limits = &limits;

  void* data;
  uint32_t num_bytes = 4;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.BeginPut(a, &num_bytes, IPCZ_NO_FLAGS, &options, &data));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.EndPut(a, num_bytes, nullptr, 0, nullptr, 0,
                                        IPCZ_NO_FLAGS, nullptr));

  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.BeginPut(a, &num_bytes, IPCZ_NO_FLAGS, &options, &data));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.EndPut(a, num_bytes, nullptr, 0, nullptr, 0,
                                        IPCZ_NO_FLAGS, nullptr));

  EXPECT_EQ(IPCZ_RESULT_RESOURCE_EXHAUSTED,
            ipcz.BeginPut(a, &num_bytes, IPCZ_NO_FLAGS, &options, &data));

  limits.max_queued_parcels = 3;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.BeginPut(a, &num_bytes, IPCZ_NO_FLAGS, &options, &data));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.EndPut(a, num_bytes, nullptr, 0, nullptr, 0,
                                        IPCZ_NO_FLAGS, nullptr));

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr));
}

TEST_F(BeginPutAPITest, DataLimit) {
  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node(), IPCZ_NO_FLAGS, nullptr, &a, &b));

  IpczPutLimits limits = {sizeof(limits)};
  limits.max_queued_bytes = 8;
  IpczBeginPutOptions options = {sizeof(options)};
  options.limits = &limits;

  void* data;
  uint32_t num_bytes = 4;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.BeginPut(a, &num_bytes, IPCZ_NO_FLAGS, &options, &data));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.EndPut(a, num_bytes, nullptr, 0, nullptr, 0,
                                        IPCZ_NO_FLAGS, nullptr));

  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.BeginPut(a, &num_bytes, IPCZ_NO_FLAGS, &options, &data));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.EndPut(a, num_bytes, nullptr, 0, nullptr, 0,
                                        IPCZ_NO_FLAGS, nullptr));

  EXPECT_EQ(IPCZ_RESULT_RESOURCE_EXHAUSTED,
            ipcz.BeginPut(a, &num_bytes, IPCZ_NO_FLAGS, &options, &data));

  limits.max_queued_bytes = 12;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.BeginPut(a, &num_bytes, IPCZ_NO_FLAGS, &options, &data));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.EndPut(a, num_bytes, nullptr, 0, nullptr, 0,
                                        IPCZ_NO_FLAGS, nullptr));

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr));
}

}  // namespace
}  // namespace ipcz
