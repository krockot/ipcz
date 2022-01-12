// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "os/memory.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using EndGetAPITest = test::APITest;

TEST_F(EndGetAPITest, InvalidArgs) {
  // Invalid port.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.EndGet(IPCZ_INVALID_HANDLE, 0, IPCZ_NO_FLAGS, nullptr, nullptr,
                        nullptr, nullptr, nullptr));

  // Larger reported data consumption than exposed by BeginGet().
  Put(q, "hi");
  const void* data;
  uint32_t num_bytes;
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.BeginGet(p, IPCZ_NO_FLAGS, nullptr, &data,
                                          &num_bytes, nullptr, nullptr));
  EXPECT_EQ(2u, num_bytes);
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.EndGet(p, num_bytes * 2, IPCZ_NO_FLAGS, nullptr, nullptr,
                        nullptr, nullptr, nullptr));
}

TEST_F(EndGetAPITest, NoGetInProgress) {
  EXPECT_EQ(IPCZ_RESULT_FAILED_PRECONDITION,
            ipcz.EndGet(q, 0, IPCZ_NO_FLAGS, nullptr, nullptr, nullptr, nullptr,
                        nullptr));
}

TEST_F(EndGetAPITest, InsufficientStorage) {
  IpczHandle portals[2];
  OpenPortals(&portals[0], &portals[1]);
  os::Handle handle = os::Memory(64).TakeHandle();
  Put(q, "hey!", portals, {&handle, 1});

  const void* data;
  uint32_t num_bytes;
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.BeginGet(p, IPCZ_NO_FLAGS, nullptr, &data,
                                          &num_bytes, nullptr, nullptr));

  EXPECT_EQ(4u, num_bytes);

  // Start with no space for anything. Counts should be updated.
  uint32_t num_portals = 0;
  uint32_t num_os_handles = 0;
  EXPECT_EQ(IPCZ_RESULT_RESOURCE_EXHAUSTED,
            ipcz.EndGet(p, num_bytes, IPCZ_NO_FLAGS, nullptr, nullptr,
                        &num_portals, nullptr, &num_os_handles));
  EXPECT_EQ(2u, num_portals);
  EXPECT_EQ(1u, num_os_handles);

  // Verify the same result when only one of the arguments is insufficient.
  num_portals = 0;
  IpczOSHandle os_handle = {sizeof(os_handle)};
  EXPECT_EQ(IPCZ_RESULT_RESOURCE_EXHAUSTED,
            ipcz.EndGet(p, num_bytes, IPCZ_NO_FLAGS, nullptr, nullptr,
                        &num_portals, &os_handle, &num_os_handles));
  EXPECT_EQ(2u, num_portals);
  EXPECT_EQ(1u, num_os_handles);

  num_os_handles = 0;
  EXPECT_EQ(IPCZ_RESULT_RESOURCE_EXHAUSTED,
            ipcz.EndGet(p, num_bytes, IPCZ_NO_FLAGS, nullptr, portals,
                        &num_portals, nullptr, &num_os_handles));
  EXPECT_EQ(2u, num_portals);
  EXPECT_EQ(1u, num_os_handles);
}

TEST_F(EndGetAPITest, Abort) {
  Put(q, "yo");

  const void* data;
  uint32_t num_bytes;
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.BeginGet(p, IPCZ_NO_FLAGS, nullptr, &data,
                                          &num_bytes, nullptr, nullptr));

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.EndGet(p, 0, IPCZ_END_GET_ABORT, nullptr,
                                        nullptr, nullptr, nullptr, nullptr));

  // Another get operation should now be able to proceed.
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.BeginGet(p, IPCZ_NO_FLAGS, nullptr, &data,
                                          &num_bytes, nullptr, nullptr));
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
                                          &num_bytes, nullptr, nullptr));
  ASSERT_TRUE(data);
  ASSERT_EQ(6u, num_bytes);
  EXPECT_EQ("ab", get_string(data, 2));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.EndGet(p, 2, IPCZ_NO_FLAGS, nullptr, nullptr,
                                        nullptr, nullptr, nullptr));

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.BeginGet(p, IPCZ_NO_FLAGS, nullptr, &data,
                                          &num_bytes, nullptr, nullptr));
  ASSERT_TRUE(data);
  ASSERT_EQ(4u, num_bytes);
  EXPECT_EQ("cd", get_string(data, 2));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.EndGet(p, 2, IPCZ_NO_FLAGS, nullptr, nullptr,
                                        nullptr, nullptr, nullptr));

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.BeginGet(p, IPCZ_NO_FLAGS, nullptr, &data,
                                          &num_bytes, nullptr, nullptr));
  ASSERT_TRUE(data);
  ASSERT_EQ(2u, num_bytes);
  EXPECT_EQ("ef", get_string(data, 2));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.EndGet(p, 2, IPCZ_NO_FLAGS, nullptr, nullptr,
                                        nullptr, nullptr, nullptr));
}

}  // namespace
}  // namespace ipcz
