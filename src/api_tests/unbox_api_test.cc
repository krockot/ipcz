// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "ipcz/ipcz.h"
#include "reference_drivers/blob.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using UnboxAPITest = test::APITest;

TEST_F(UnboxAPITest, InvalidArgs) {
  IpczHandle handle;
  IpczDriverHandle driver_handle;

  EXPECT_EQ(
      IPCZ_RESULT_INVALID_ARGUMENT,
      ipcz.Unbox(IPCZ_INVALID_HANDLE, IPCZ_NO_FLAGS, nullptr, &driver_handle));

  IpczDriverHandle blob = reference_drivers::Blob::Create("merp");
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.Box(node, blob, IPCZ_NO_FLAGS, nullptr, &handle));
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Unbox(handle, IPCZ_NO_FLAGS, nullptr, nullptr));

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Close(handle, IPCZ_NO_FLAGS, nullptr));
}

}  // namespace
}  // namespace ipcz
