// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using CloseAPITest = test::APITest;

TEST_F(CloseAPITest, InvalidArgs) {
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.Close(IPCZ_INVALID_HANDLE, IPCZ_NO_FLAGS, nullptr));
}

}  // namespace
}  // namespace ipcz
