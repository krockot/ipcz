// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using PutAPITest = test::APITest;

TEST_F(PutAPITest, PutData) {
  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node(), IPCZ_NO_FLAGS, nullptr, &a, &b));

  uint8_t data[] = {1, 2, 3, 4};
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Put(a, data, 4, nullptr, 0, nullptr, 0,
                                     IPCZ_NO_FLAGS, nullptr));

  IpczPortalStatus status = {sizeof(status)};
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.QueryPortalStatus(b, IPCZ_PORTAL_STATUS_FIELD_LOCAL_PARCELS,
                                   IPCZ_NO_FLAGS, nullptr, &status));
  EXPECT_EQ(1u, status.num_local_parcels);

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr));
}

TEST_F(PutAPITest, PutClosed) {
  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node(), IPCZ_NO_FLAGS, nullptr, &a, &b));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_NOT_FOUND, ipcz.Put(a, nullptr, 0, nullptr, 0, nullptr,
                                            0, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr));
}

TEST_F(PutAPITest, PutParcelLimit) {
  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node(), IPCZ_NO_FLAGS, nullptr, &a, &b));

  IpczPutLimits limits = {sizeof(limits)};
  limits.max_queued_parcels = 2;
  IpczPutOptions options = {sizeof(options)};
  options.limits = &limits;

  const uint32_t data = 42;
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Put(a, &data, 4, nullptr, 0, nullptr, 0,
                                     IPCZ_NO_FLAGS, &options));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Put(a, &data, 4, nullptr, 0, nullptr, 0,
                                     IPCZ_NO_FLAGS, &options));

  EXPECT_EQ(
      IPCZ_RESULT_RESOURCE_EXHAUSTED,
      ipcz.Put(a, &data, 4, nullptr, 0, nullptr, 0, IPCZ_NO_FLAGS, &options));

  limits.max_queued_parcels = 3;
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Put(a, &data, 4, nullptr, 0, nullptr, 0,
                                     IPCZ_NO_FLAGS, &options));

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr));
}

TEST_F(PutAPITest, PutDataLimit) {
  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node(), IPCZ_NO_FLAGS, nullptr, &a, &b));

  IpczPutLimits limits = {sizeof(limits)};
  limits.max_queued_bytes = 8;
  IpczPutOptions options = {sizeof(options)};
  options.limits = &limits;

  const uint32_t data = 42;
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Put(a, &data, 4, nullptr, 0, nullptr, 0,
                                     IPCZ_NO_FLAGS, &options));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Put(a, &data, 4, nullptr, 0, nullptr, 0,
                                     IPCZ_NO_FLAGS, &options));

  EXPECT_EQ(
      IPCZ_RESULT_RESOURCE_EXHAUSTED,
      ipcz.Put(a, &data, 4, nullptr, 0, nullptr, 0, IPCZ_NO_FLAGS, &options));

  limits.max_queued_bytes = 12;
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Put(a, &data, 4, nullptr, 0, nullptr, 0,
                                     IPCZ_NO_FLAGS, &options));

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr));
}

TEST_F(PutAPITest, TwoPhaseNoOverlap) {
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

TEST_F(PutAPITest, TwoPhaseEndNonExistent) {
  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node(), IPCZ_NO_FLAGS, nullptr, &a, &b));

  EXPECT_EQ(IPCZ_RESULT_FAILED_PRECONDITION,
            ipcz.EndPut(a, 4, nullptr, 0, nullptr, 0, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(
      IPCZ_RESULT_FAILED_PRECONDITION,
      ipcz.EndPut(a, 4, nullptr, 0, nullptr, 0, IPCZ_END_PUT_ABORT, nullptr));

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr));
}

TEST_F(PutAPITest, TwoPhasePutParcelLimit) {
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

TEST_F(PutAPITest, TwoPhaseOversizedEnd) {
  IpczHandle a, b;
  ipcz.OpenPortals(node(), IPCZ_NO_FLAGS, nullptr, &a, &b);
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.BeginPut(a, nullptr, IPCZ_NO_FLAGS, nullptr, nullptr));
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.EndPut(a, 4, nullptr, 0, nullptr, 0, IPCZ_NO_FLAGS, nullptr));
  ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr);
  ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr);
}

}  // namespace
}  // namespace ipcz
