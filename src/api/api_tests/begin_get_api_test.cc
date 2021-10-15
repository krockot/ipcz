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

}  // namespace
}  // namespace ipcz
