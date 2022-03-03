// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "reference_drivers/single_process_reference_driver.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using ConnectNodeAPITest = test::APITest;

TEST_F(ConnectNodeAPITest, InvalidArgs) {
  IpczHandle portal;
  IpczDriverHandle transports[2];
  EXPECT_EQ(IPCZ_RESULT_OK,
            reference_drivers::kSingleProcessReferenceDriver.CreateTransports(
                IPCZ_INVALID_HANDLE, IPCZ_NO_FLAGS, nullptr, &transports[0],
                &transports[1]));

  // Invalid node.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.ConnectNode(IPCZ_INVALID_HANDLE, transports[0], 1,
                             IPCZ_NO_FLAGS, nullptr, &portal));

  // Zero initial portals.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.ConnectNode(node, transports[0], 0, IPCZ_NO_FLAGS, nullptr,
                             &portal));

  // Null portal storage.
  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.ConnectNode(node, transports[0], 1, IPCZ_NO_FLAGS, nullptr,
                             nullptr));

  IpczHandle broker;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.CreateNode(&reference_drivers::kSingleProcessReferenceDriver,
                            IPCZ_INVALID_HANDLE, IPCZ_CREATE_NODE_AS_BROKER,
                            nullptr, &broker));

  // Non-brokers cannot specify IPCZ_CONNECT_NODE_INHERIT_BROKER or
  // IPCZ_CONNECT_NODE_SHARE_BROKER.
  EXPECT_EQ(
      IPCZ_RESULT_INVALID_ARGUMENT,
      ipcz.ConnectNode(broker, transports[0], 1,
                       IPCZ_CONNECT_NODE_INHERIT_BROKER, nullptr, &portal));

  EXPECT_EQ(IPCZ_RESULT_INVALID_ARGUMENT,
            ipcz.ConnectNode(broker, transports[0], 1,
                             IPCZ_CONNECT_NODE_SHARE_BROKER, nullptr, &portal));
}

}  // namespace
}  // namespace ipcz
