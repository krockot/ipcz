// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/multinode_test.h"

#include "ipcz/ipcz.h"
#include "reference_drivers/multiprocess_reference_driver.h"
#include "reference_drivers/single_process_reference_driver.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "util/os_process.h"

namespace ipcz {
namespace test {

namespace {

const IpczDriver* GetDriver(MultinodeTest::DriverMode mode) {
  switch (mode) {
    case MultinodeTest::DriverMode::kSync:
      return &reference_drivers::kSingleProcessReferenceDriver;

    case MultinodeTest::DriverMode::kAsync:
      return &reference_drivers::kMultiprocessReferenceDriver;

    case ipcz::test::MultinodeTest::DriverMode::kAsyncDelegatedAlloc:
      return &reference_drivers::kMultiprocessReferenceDriver;
  }
}

IpczConnectNodeFlags GetExtraNonBrokerFlags(MultinodeTest::DriverMode mode) {
  if (mode == ipcz::test::MultinodeTest::DriverMode::kAsyncDelegatedAlloc) {
    return IPCZ_CONNECT_NODE_TO_ALLOCATION_DELEGATE;
  }
  return IPCZ_NO_FLAGS;
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

IpczHandle MultinodeTest::ConnectToBroker(DriverMode mode,
                                          IpczHandle non_broker_node,
                                          IpczDriverHandle transport) {
  IpczHandle portal;
  IpczConnectNodeFlags flags = IPCZ_CONNECT_NODE_TO_BROKER;
  flags |= GetExtraNonBrokerFlags(mode);
  IpczResult result = ipcz.ConnectNode(non_broker_node, transport, nullptr, 1,
                                       flags, nullptr, &portal);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
  return portal;
}

IpczHandle MultinodeTest::ConnectToNonBroker(IpczHandle broker_node,
                                             IpczDriverHandle transport) {
  IpczOSProcessHandle process = {sizeof(process)};
  OSProcess::ToIpczOSProcessHandle(OSProcess::GetCurrent(), process);

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
  *non_broker_portal = ConnectToBroker(mode, non_broker_node, transport1);
}

}  // namespace test
}  // namespace ipcz
