// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "reference_drivers/memory.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using GetAPITest = test::APITest;

TEST_F(GetAPITest, InvalidArgs) {
  // Invalid portal.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Get(IPCZ_INVALID_HANDLE, IPCZ_NO_FLAGS, nullptr, nullptr,
                     nullptr, nullptr, nullptr, nullptr, nullptr));

  uint32_t not_zero = 4;

  // Null data buffer but non-zero byte count
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Get(q, IPCZ_NO_FLAGS, nullptr, nullptr, &not_zero, nullptr,
                     nullptr, nullptr, nullptr));

  // Null portal buffer but non-zero portal count.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Get(q, IPCZ_NO_FLAGS, nullptr, nullptr, nullptr, nullptr,
                     &not_zero, nullptr, nullptr));

  // Null OS handle buffer but non-zero OS handle count.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Get(q, IPCZ_NO_FLAGS, nullptr, nullptr, nullptr, nullptr,
                     nullptr, nullptr, &not_zero));
}

TEST_F(GetAPITest, InsufficientStorage) {
  uint32_t data[] = {1, 2, 3, 4};

  IpczHandle portals[2];
  OpenPortals(&portals[0], &portals[1]);

  IpczOSHandle os_handle = {sizeof(os_handle)};
  OSHandle::ToIpczOSHandle(reference_drivers::Memory(64).TakeHandle(),
                           &os_handle);

  ASSERT_EQ(IPCZ_RESULT_OK, ipcz.Put(q, data, sizeof(data), portals, 2,
                                     &os_handle, 1, IPCZ_NO_FLAGS, nullptr));

  // Start with no space for anything. All counts should be updated.
  uint32_t num_bytes = 0;
  uint32_t num_portals = 0;
  uint32_t num_os_handles = 0;
  EXPECT_EQ(IPCZ_RESULT_RESOURCE_EXHAUSTED,
            ipcz.Get(p, IPCZ_NO_FLAGS, nullptr, nullptr, &num_bytes, nullptr,
                     &num_portals, nullptr, &num_os_handles));
  EXPECT_EQ(sizeof(data), num_bytes);
  EXPECT_EQ(2u, num_portals);
  EXPECT_EQ(1u, num_os_handles);

  // Verify the same result when only one of the arguments is insufficient.
  num_bytes = 0;
  EXPECT_EQ(IPCZ_RESULT_RESOURCE_EXHAUSTED,
            ipcz.Get(p, IPCZ_NO_FLAGS, nullptr, data, &num_bytes, portals,
                     &num_portals, &os_handle, &num_os_handles));
  EXPECT_EQ(sizeof(data), num_bytes);
  EXPECT_EQ(2u, num_portals);
  EXPECT_EQ(1u, num_os_handles);

  num_portals = 0;
  EXPECT_EQ(IPCZ_RESULT_RESOURCE_EXHAUSTED,
            ipcz.Get(p, IPCZ_NO_FLAGS, nullptr, data, &num_bytes, portals,
                     &num_portals, &os_handle, &num_os_handles));
  EXPECT_EQ(sizeof(data), num_bytes);
  EXPECT_EQ(2u, num_portals);
  EXPECT_EQ(1u, num_os_handles);

  num_os_handles = 0;
  EXPECT_EQ(IPCZ_RESULT_RESOURCE_EXHAUSTED,
            ipcz.Get(p, IPCZ_NO_FLAGS, nullptr, data, &num_bytes, portals,
                     &num_portals, &os_handle, &num_os_handles));
  EXPECT_EQ(sizeof(data), num_bytes);
  EXPECT_EQ(2u, num_portals);
  EXPECT_EQ(1u, num_os_handles);
}

TEST_F(GetAPITest, OutputExactDimensionsOnSuccess) {
  uint32_t data[] = {1, 2, 3, 4};

  IpczHandle portals[2];
  OpenPortals(&portals[0], &portals[1]);

  IpczOSHandle os_handle = {sizeof(os_handle)};
  OSHandle::ToIpczOSHandle(reference_drivers::Memory(64).TakeHandle(),
                           &os_handle);

  ASSERT_EQ(IPCZ_RESULT_OK, ipcz.Put(q, data, sizeof(data), portals, 2,
                                     &os_handle, 1, IPCZ_NO_FLAGS, nullptr));

  // If we provide more than enough storage for the parcel, our inputs should be
  // updated to reflect the actual sizes.
  uint32_t out_data[8];
  uint32_t num_bytes = sizeof(out_data);
  IpczHandle out_portals[4];
  uint32_t num_portals = 4;
  IpczOSHandle out_os_handles[2];
  out_os_handles[0].size = sizeof(IpczOSHandle);
  uint32_t num_os_handles = 2;
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Get(p, IPCZ_NO_FLAGS, nullptr, out_data,
                                     &num_bytes, out_portals, &num_portals,
                                     out_os_handles, &num_os_handles));
  EXPECT_EQ(sizeof(data), num_bytes);
  EXPECT_EQ(2u, num_portals);
  EXPECT_EQ(1u, num_os_handles);

  ClosePortals({out_portals[0], out_portals[1]});
}

TEST_F(GetAPITest, Partial) {
  uint32_t data[] = {1, 2, 3, 4, 5};

  IpczHandle portals[2];
  OpenPortals(&portals[0], &portals[1]);

  IpczOSHandle os_handle = {sizeof(os_handle)};
  OSHandle::ToIpczOSHandle(reference_drivers::Memory(64).TakeHandle(),
                           &os_handle);

  ASSERT_EQ(IPCZ_RESULT_OK, ipcz.Put(q, data, sizeof(data), portals, 2,
                                     &os_handle, 1, IPCZ_NO_FLAGS, nullptr));

  // Insufficient storage with IPCZ_GET_PARTIAL should still result in a
  // successful Get().
  uint32_t out_data[2];
  uint32_t num_bytes = sizeof(out_data);
  IpczHandle out_handle;
  uint32_t num_handles = 1;
  IpczOSHandle out_os_handle = {.size = sizeof(out_os_handle)};
  uint32_t num_os_handles = 1;
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Get(p, IPCZ_GET_PARTIAL, nullptr, out_data,
                                     &num_bytes, &out_handle, &num_handles,
                                     &out_os_handle, &num_os_handles));
  EXPECT_EQ(sizeof(out_data), num_bytes);
  EXPECT_EQ(1u, out_data[0]);
  EXPECT_EQ(2u, out_data[1]);
  EXPECT_EQ(1u, num_handles);
  EXPECT_EQ(1u, num_os_handles);
  CloseHandles({out_handle});

  std::ignore = OSHandle::FromIpczOSHandle(out_os_handle);

  // A second partial read should grab more stuff from the same parcel.
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Get(p, IPCZ_GET_PARTIAL, nullptr, out_data,
                                     &num_bytes, &out_handle, &num_handles,
                                     &out_os_handle, &num_os_handles));
  EXPECT_EQ(sizeof(out_data), num_bytes);
  EXPECT_EQ(3u, out_data[0]);
  EXPECT_EQ(4u, out_data[1]);
  EXPECT_EQ(1u, num_handles);
  EXPECT_EQ(0u, num_os_handles);
  CloseHandles({out_handle});

  // One final partial read to get the last of the data.
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Get(p, IPCZ_GET_PARTIAL, nullptr, out_data,
                                     &num_bytes, &out_handle, &num_handles,
                                     &out_os_handle, &num_os_handles));
  EXPECT_EQ(sizeof(out_data[0]), num_bytes);
  EXPECT_EQ(5u, out_data[0]);
  EXPECT_EQ(0u, num_handles);
  EXPECT_EQ(0u, num_os_handles);

  // Now the portal is empty, so there's nothing to read even partially.
  EXPECT_EQ(
      IPCZ_RESULT_UNAVAILABLE,
      ipcz.Get(p, IPCZ_GET_PARTIAL, nullptr, out_data, &num_bytes, &out_handle,
               &num_handles, &out_os_handle, &num_os_handles));
}

TEST_F(GetAPITest, Empty) {
  EXPECT_EQ(IPCZ_RESULT_UNAVAILABLE,
            ipcz.Get(q, IPCZ_NO_FLAGS, nullptr, nullptr, nullptr, nullptr,
                     nullptr, nullptr, nullptr));
}

TEST_F(GetAPITest, Dead) {
  IpczHandle a, b;
  OpenPortals(&a, &b);
  ClosePortals({b});
  EXPECT_EQ(IPCZ_RESULT_NOT_FOUND,
            ipcz.Get(a, IPCZ_NO_FLAGS, nullptr, nullptr, nullptr, nullptr,
                     nullptr, nullptr, nullptr));
  ClosePortals({a});
}

}  // namespace
}  // namespace ipcz
