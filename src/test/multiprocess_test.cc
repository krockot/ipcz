// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/multiprocess_test.h"

#include "ipcz/ipcz.h"
#include "reference_drivers/channel.h"
#include "reference_drivers/multiprocess_reference_driver.h"
#include "test/test_client.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace test {

MultiprocessTest::MultiprocessTest() {
  IpczCreateNodeFlags flags = IPCZ_NO_FLAGS;
  if (!TestClient::InClientProcess()) {
    flags = IPCZ_CREATE_NODE_AS_BROKER;
  }
  IpczResult result =
      ipcz.CreateNode(&reference_drivers::kMultiprocessReferenceDriver,
                      IPCZ_INVALID_DRIVER_HANDLE, flags, nullptr, &node);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
}

MultiprocessTest::~MultiprocessTest() {
  IpczResult result = ipcz.Close(node, IPCZ_NO_FLAGS, nullptr);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
}

IpczHandle MultiprocessTest::ConnectToClient(TestClient& client) {
  IpczHandle portal;
  IpczResult result =
      ipcz.ConnectNode(node,
                       reference_drivers::CreateTransportFromChannel(
                           std::move(client.channel())),
                       1, IPCZ_NO_FLAGS, nullptr, &portal);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
  return portal;
}

IpczHandle MultiprocessTest::ConnectToBroker(
    reference_drivers::Channel& channel) {
  IpczHandle portal;
  IpczResult result = ipcz.ConnectNode(
      node, reference_drivers::CreateTransportFromChannel(std::move(channel)),
      1, IPCZ_CONNECT_NODE_TO_BROKER, nullptr, &portal);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
  return portal;
}

}  // namespace test
}  // namespace ipcz
