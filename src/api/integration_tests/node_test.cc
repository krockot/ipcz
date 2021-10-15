// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using NodeTest = test::APITest;

TEST_F(NodeTest, CreateAndDestroyNode) {
  IpczHandle node;
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.CreateNode(IPCZ_NO_FLAGS, nullptr, &node));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.DestroyNode(node, IPCZ_NO_FLAGS, nullptr));
}

TEST_F(NodeTest, CreateAndDestroyBrokerNode) {
  IpczHandle node;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.CreateNode(IPCZ_CREATE_NODE_AS_BROKER, nullptr, &node));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.DestroyNode(node, IPCZ_NO_FLAGS, nullptr));
}

TEST_F(NodeTest, OpenAndClosePortals) {
  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node(), IPCZ_NO_FLAGS, nullptr, &a, &b));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr));
}

}  // namespace
}  // namespace ipcz
