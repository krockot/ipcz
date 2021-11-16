// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/multinode_test.h"

#include "drivers/multiprocess_reference_driver.h"
#include "drivers/single_process_reference_driver.h"
#include "ipcz/ipcz.h"
#include "os/process.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace test {

namespace {

const IpczDriver* GetDriver(MultinodeTest::DriverMode mode) {
  switch (mode) {
    case MultinodeTest::DriverMode::kSynchronous:
      return &drivers::kSingleProcessReferenceDriver;

    case MultinodeTest::DriverMode::kAsynchronous:
      return &drivers::kMultiprocessReferenceDriver;
  }
}

}  // namespace

MultinodeTest::MultinodeTest() = default;

MultinodeTest::~MultinodeTest() = default;

IpczHandle MultinodeTest::CreateBrokerNode(DriverMode mode) {
  IpczHandle node;
  ipcz.CreateNode(GetDriver(mode), IPCZ_INVALID_DRIVER_HANDLE,
                  IPCZ_CREATE_NODE_AS_BROKER, nullptr, &node);
  return node;
}

IpczHandle MultinodeTest::CreateNonBrokerNode(DriverMode mode) {
  IpczHandle node;
  ipcz.CreateNode(GetDriver(mode), IPCZ_INVALID_DRIVER_HANDLE, IPCZ_NO_FLAGS,
                  nullptr, &node);
  return node;
}

void MultinodeTest::CreateTransports(DriverMode mode,
                                     IpczDriverHandle* transport0,
                                     IpczDriverHandle* transport1) {
  IpczResult result = GetDriver(mode)->CreateTransports(
      IPCZ_INVALID_DRIVER_HANDLE, IPCZ_NO_FLAGS, nullptr, transport0,
      transport1);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
}

IpczHandle MultinodeTest::ConnectToBroker(IpczHandle non_broker_node,
                                          IpczDriverHandle transport) {
  IpczHandle portal;
  IpczResult result =
      ipcz.ConnectNode(non_broker_node, transport, nullptr, 1,
                       IPCZ_CONNECT_NODE_TO_BROKER, nullptr, &portal);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
  return portal;
}

IpczHandle MultinodeTest::ConnectToNonBroker(IpczHandle broker_node,
                                             IpczDriverHandle transport) {
  IpczOSProcessHandle process = {sizeof(process)};
  os::Process::ToIpczOSProcessHandle(os::Process::GetCurrent(), process);

  IpczHandle portal;
  IpczResult result = ipcz.ConnectNode(broker_node, transport, &process, 1,
                                       IPCZ_NO_FLAGS, nullptr, &portal);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
  return portal;
}

void MultinodeTest::Connect(IpczHandle broker_node,
                            IpczHandle non_broker_node,
                            DriverMode mode,
                            IpczHandle* broker_portal,
                            IpczHandle* non_broker_portal) {
  IpczDriverHandle transport0;
  IpczDriverHandle transport1;
  CreateTransports(mode, &transport0, &transport1);

  *broker_portal = ConnectToNonBroker(broker_node, transport0);
  *non_broker_portal = ConnectToBroker(non_broker_node, transport1);
}

}  // namespace test
}  // namespace ipcz