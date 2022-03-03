// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using PutAPITest = test::APITest;

TEST_F(PutAPITest, InvalidArgs) {
  // Invalid portal
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Put(IPCZ_INVALID_HANDLE, nullptr, 0, nullptr, 0, IPCZ_NO_FLAGS,
                     nullptr));

  IpczHandle a, b;
  ipcz.OpenPortals(node, IPCZ_NO_FLAGS, nullptr, &a, &b);

  // Invalid options
  IpczPutOptions options = {0};
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Put(a, nullptr, 0, nullptr, 0, IPCZ_NO_FLAGS, &options));

  // Invalid limits
  IpczPutLimits limits = {0};
  options.size = sizeof(options);
  options.limits = &limits;
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Put(a, nullptr, 0, nullptr, 0, IPCZ_NO_FLAGS, &options));

  // Null buffers with non-zero counts.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Put(a, nullptr, 1, nullptr, 0, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Put(a, nullptr, 0, nullptr, 1, IPCZ_NO_FLAGS, nullptr));

  // Putting a portal or its peer into itself is also invalid.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Put(a, nullptr, 0, &a, 1, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Put(a, nullptr, 0, &b, 1, IPCZ_NO_FLAGS, nullptr));

  ipcz.Close(a, IPCZ_NO_FLAGS, nullptr);
  ipcz.Close(b, IPCZ_NO_FLAGS, nullptr);
}

TEST_F(PutAPITest, PutData) {
  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node, IPCZ_NO_FLAGS, nullptr, &a, &b));

  uint8_t data[] = {1, 2, 3, 4};
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.Put(a, data, 4, nullptr, 0, IPCZ_NO_FLAGS, nullptr));

  IpczPortalStatus status = {sizeof(status)};
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.QueryPortalStatus(b, IPCZ_NO_FLAGS, nullptr, &status));
  EXPECT_EQ(1u, status.num_local_parcels);

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Close(a, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Close(b, IPCZ_NO_FLAGS, nullptr));
}

TEST_F(PutAPITest, PutClosed) {
  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node, IPCZ_NO_FLAGS, nullptr, &a, &b));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Close(b, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_NOT_FOUND,
            ipcz.Put(a, nullptr, 0, nullptr, 0, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Close(a, IPCZ_NO_FLAGS, nullptr));
}

TEST_F(PutAPITest, PutParcelLimit) {
  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node, IPCZ_NO_FLAGS, nullptr, &a, &b));

  IpczPutLimits limits = {sizeof(limits)};
  limits.max_queued_parcels = 2;
  limits.max_queued_bytes = 0xfffffffflu;
  IpczPutOptions options = {sizeof(options)};
  options.limits = &limits;

  const uint32_t data = 42;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.Put(a, &data, 4, nullptr, 0, IPCZ_NO_FLAGS, &options));
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.Put(a, &data, 4, nullptr, 0, IPCZ_NO_FLAGS, &options));

  EXPECT_EQ(IPCZ_RESULT_RESOURCE_EXHAUSTED,
            ipcz.Put(a, &data, 4, nullptr, 0, IPCZ_NO_FLAGS, &options));

  limits.max_queued_parcels = 3;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.Put(a, &data, 4, nullptr, 0, IPCZ_NO_FLAGS, &options));

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Close(a, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Close(b, IPCZ_NO_FLAGS, nullptr));
}

TEST_F(PutAPITest, PutDataLimit) {
  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node, IPCZ_NO_FLAGS, nullptr, &a, &b));

  IpczPutLimits limits = {sizeof(limits)};
  limits.max_queued_parcels = 0xfffffffful;
  limits.max_queued_bytes = 8;
  IpczPutOptions options = {sizeof(options)};
  options.limits = &limits;

  const uint32_t data = 42;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.Put(a, &data, 4, nullptr, 0, IPCZ_NO_FLAGS, &options));
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.Put(a, &data, 4, nullptr, 0, IPCZ_NO_FLAGS, &options));

  EXPECT_EQ(IPCZ_RESULT_RESOURCE_EXHAUSTED,
            ipcz.Put(a, &data, 4, nullptr, 0, IPCZ_NO_FLAGS, &options));

  limits.max_queued_bytes = 12;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.Put(a, &data, 4, nullptr, 0, IPCZ_NO_FLAGS, &options));

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Close(a, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Close(b, IPCZ_NO_FLAGS, nullptr));
}

}  // namespace
}  // namespace ipcz
