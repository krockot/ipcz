// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "reference_drivers/single_process_reference_driver.h"
#include "test/test_base.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using NodeTest = test::TestBase;

TEST_F(NodeTest, CreateAndDestroyNode) {
  IpczHandle node;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.CreateNode(&reference_drivers::kSingleProcessReferenceDriver,
                            IPCZ_INVALID_DRIVER_HANDLE, IPCZ_NO_FLAGS, nullptr,
                            &node));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Close(node, IPCZ_NO_FLAGS, nullptr));
}

TEST_F(NodeTest, CreateAndDestroyBrokerNode) {
  IpczHandle node;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.CreateNode(&reference_drivers::kSingleProcessReferenceDriver,
                            IPCZ_INVALID_DRIVER_HANDLE,
                            IPCZ_CREATE_NODE_AS_BROKER, nullptr, &node));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Close(node, IPCZ_NO_FLAGS, nullptr));
}

TEST_F(NodeTest, OpenAndClosePortals) {
  IpczHandle node;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.CreateNode(&reference_drivers::kSingleProcessReferenceDriver,
                            IPCZ_INVALID_DRIVER_HANDLE, IPCZ_NO_FLAGS, nullptr,
                            &node));
  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node, IPCZ_NO_FLAGS, nullptr, &a, &b));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Close(a, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Close(b, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Close(node, IPCZ_NO_FLAGS, nullptr));
}

}  // namespace
}  // namespace ipcz
