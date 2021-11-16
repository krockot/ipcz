// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/multiprocess_test.h"

#include "drivers/multiprocess_reference_driver.h"
#include "ipcz/ipcz.h"
#include "os/channel.h"
#include "os/handle.h"
#include "os/process.h"
#include "test/test_client.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace test {

namespace {

IpczDriverHandle CreateTransportFromChannel(os::Channel& channel) {
  IpczDriverHandle transport;
  IpczOSHandle os_handle = {sizeof(os_handle)};
  os::Handle::ToIpczOSHandle(channel.TakeHandle(), &os_handle);
  IpczResult result =
      drivers::kMultiprocessReferenceDriver.DeserializeTransport(
          IPCZ_INVALID_DRIVER_HANDLE, nullptr, 0, &os_handle, 1, nullptr,
          IPCZ_NO_FLAGS, nullptr, &transport);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
  return transport;
}

}  // namespace

MultiprocessTest::MultiprocessTest() {
  IpczCreateNodeFlags flags = IPCZ_NO_FLAGS;
  if (!TestClient::InClientProcess()) {
    flags = IPCZ_CREATE_NODE_AS_BROKER;
  }
  IpczResult result =
      ipcz.CreateNode(&drivers::kMultiprocessReferenceDriver,
                      IPCZ_INVALID_DRIVER_HANDLE, flags, nullptr, &node);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
}

MultiprocessTest::~MultiprocessTest() {
  IpczResult result = ipcz.DestroyNode(node, IPCZ_NO_FLAGS, nullptr);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
}

IpczHandle MultiprocessTest::ConnectToClient(TestClient& client) {
  IpczOSProcessHandle process = {sizeof(process)};
  os::Process::ToIpczOSProcessHandle(client.process().Clone(), process);

  IpczHandle portal;
  IpczResult result =
      ipcz.ConnectNode(node, CreateTransportFromChannel(client.channel()),
                       &process, 1, IPCZ_NO_FLAGS, nullptr, &portal);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
  return portal;
}

IpczHandle MultiprocessTest::ConnectToBroker(os::Channel& channel) {
  IpczHandle portal;
  IpczResult result =
      ipcz.ConnectNode(node, CreateTransportFromChannel(channel), nullptr, 1,
                       IPCZ_CONNECT_NODE_TO_BROKER, nullptr, &portal);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
  return portal;
}

}  // namespace test
}  // namespace ipcz
