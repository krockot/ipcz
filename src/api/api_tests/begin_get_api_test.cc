// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using BeginGetAPITest = test::APITest;

TEST_F(BeginGetAPITest, InvalidArgs) {
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.BeginGet(IPCZ_INVALID_HANDLE, IPCZ_NO_FLAGS, nullptr, nullptr,
                          nullptr, nullptr, nullptr));
}

TEST_F(BeginGetAPITest, NoOverlap) {
  Put(q, "hihihi");

  const void* data;
  uint32_t num_bytes = 16;
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.BeginGet(p, IPCZ_NO_FLAGS, nullptr, &data,
                                          &num_bytes, nullptr, nullptr));

  EXPECT_EQ(IPCZ_RESULT_ALREADY_EXISTS,
            ipcz.BeginGet(p, IPCZ_NO_FLAGS, nullptr, &data, &num_bytes, nullptr,
                          nullptr));
  EXPECT_EQ(IPCZ_RESULT_ALREADY_EXISTS,
            ipcz.Get(p, IPCZ_NO_FLAGS, nullptr, &data, &num_bytes, nullptr,
                     nullptr, nullptr, nullptr));
}

TEST_F(BeginGetAPITest, NoStorage) {
  Put(q, "hello");

  EXPECT_EQ(IPCZ_RESULT_RESOURCE_EXHAUSTED,
            ipcz.BeginGet(p, IPCZ_NO_FLAGS, nullptr, nullptr, nullptr, nullptr,
                          nullptr));

  const void* data;
  EXPECT_EQ(IPCZ_RESULT_RESOURCE_EXHAUSTED,
            ipcz.BeginGet(p, IPCZ_NO_FLAGS, nullptr, &data, nullptr, nullptr,
                          nullptr));

  uint32_t num_bytes;
  EXPECT_EQ(IPCZ_RESULT_RESOURCE_EXHAUSTED,
            ipcz.BeginGet(p, IPCZ_NO_FLAGS, nullptr, nullptr, &num_bytes,
                          nullptr, nullptr));
  EXPECT_EQ(5u, num_bytes);
}

TEST_F(BeginGetAPITest, Empty) {
  const void* data;
  uint32_t num_bytes;
  EXPECT_EQ(IPCZ_RESULT_UNAVAILABLE,
            ipcz.BeginGet(q, IPCZ_NO_FLAGS, nullptr, &data, &num_bytes, nullptr,
                          nullptr));
}

TEST_F(BeginGetAPITest, Dead) {
  IpczHandle a, b;
  OpenPortals(&a, &b);

  ClosePortals({b});
  const void* data;
  uint32_t num_bytes;
  EXPECT_EQ(IPCZ_RESULT_NOT_FOUND,
            ipcz.BeginGet(a, IPCZ_NO_FLAGS, nullptr, &data, &num_bytes, nullptr,
                          nullptr));
  ClosePortals({a});
}

}  // namespace
}  // namespace ipcz
