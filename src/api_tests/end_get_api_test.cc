// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using EndGetAPITest = test::APITest;

TEST_F(EndGetAPITest, InvalidArgs) {
  // Invalid portal.
  EXPECT_EQ(
      IPCZ_RESULT_INVALID_ARGUMENT,
      ipcz.EndGet(IPCZ_INVALID_HANDLE, 0, 0, IPCZ_NO_FLAGS, nullptr, nullptr));
}

TEST_F(EndGetAPITest, OutOfRange) {
  IpczHandle portals[2];
  OpenPortals(&portals[0], &portals[1]);
  Put(q, "hey!", portals);

  const void* data;
  uint32_t num_bytes;
  uint32_t num_handles;
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.BeginGet(p, IPCZ_NO_FLAGS, nullptr, &data,
                                          &num_bytes, &num_handles));
  EXPECT_EQ(4u, num_bytes);
  EXPECT_EQ(2u, num_handles);

  EXPECT_EQ(IPCZ_RESULT_OUT_OF_RANGE,
            ipcz.EndGet(p, num_bytes * 2, 0, IPCZ_NO_FLAGS, nullptr, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OUT_OF_RANGE,
            ipcz.EndGet(p, 0, 3, IPCZ_NO_FLAGS, nullptr, portals));

  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.EndGet(p, num_bytes, 2, IPCZ_NO_FLAGS, nullptr, portals));

  CloseHandles(portals);
}

TEST_F(EndGetAPITest, NoGetInProgress) {
  EXPECT_EQ(IPCZ_RESULT_FAILED_PRECONDITION,
            ipcz.EndGet(q, 0, 0, IPCZ_NO_FLAGS, nullptr, nullptr));
}

TEST_F(EndGetAPITest, Abort) {
  Put(q, "yo");

  const void* data;
  uint32_t num_bytes;
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.BeginGet(p, IPCZ_NO_FLAGS, nullptr, &data,
                                          &num_bytes, nullptr));

  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.EndGet(p, 0, 0, IPCZ_END_GET_ABORT, nullptr, nullptr));

  // Another get operation should now be able to proceed.
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.BeginGet(p, IPCZ_NO_FLAGS, nullptr, &data,
                                          &num_bytes, nullptr));
}

TEST_F(EndGetAPITest, PartialData) {
  Put(q, "abcdef");

  const auto get_string = [](const void* data, size_t length) -> std::string {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    return std::string(bytes, bytes + length);
  };

  const void* data = nullptr;
  uint32_t num_bytes = 0;
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.BeginGet(p, IPCZ_NO_FLAGS, nullptr, &data,
                                          &num_bytes, nullptr));
  ASSERT_TRUE(data);
  ASSERT_EQ(6u, num_bytes);
  EXPECT_EQ("ab", get_string(data, 2));
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.EndGet(p, 2, 0, IPCZ_NO_FLAGS, nullptr, nullptr));

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.BeginGet(p, IPCZ_NO_FLAGS, nullptr, &data,
                                          &num_bytes, nullptr));
  ASSERT_TRUE(data);
  ASSERT_EQ(4u, num_bytes);
  EXPECT_EQ("cd", get_string(data, 2));
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.EndGet(p, 2, 0, IPCZ_NO_FLAGS, nullptr, nullptr));

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.BeginGet(p, IPCZ_NO_FLAGS, nullptr, &data,
                                          &num_bytes, nullptr));
  ASSERT_TRUE(data);
  ASSERT_EQ(2u, num_bytes);
  EXPECT_EQ("ef", get_string(data, 2));
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.EndGet(p, 2, 0, IPCZ_NO_FLAGS, nullptr, nullptr));
}

}  // namespace
}  // namespace ipcz
