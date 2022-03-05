// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/multinode_test.h"

#include "ipcz/ipcz.h"
#include "reference_drivers/channel.h"
#include "reference_drivers/multiprocess_reference_driver.h"
#include "reference_drivers/object.h"
#include "reference_drivers/os_process.h"
#include "reference_drivers/single_process_reference_driver.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz::test {

namespace {

const IpczDriver* GetDriver(MultinodeTest::DriverMode mode) {
  switch (mode) {
    case MultinodeTest::DriverMode::kSync:
      return &reference_drivers::kSingleProcessReferenceDriver;

    case MultinodeTest::DriverMode::kAsync:
    case MultinodeTest::DriverMode::kAsyncDelegatedAlloc:
      return &reference_drivers::kMultiprocessReferenceDriver;

    case MultinodeTest::DriverMode::kAsyncObjectBrokering:
    case MultinodeTest::DriverMode::kAsyncObjectBrokeringAndDelegatedAlloc:
      return &reference_drivers::
          kMultiprocessReferenceDriverWithForcedObjectBrokering;
  }
}

bool UseDelegatedAlloc(MultinodeTest::DriverMode mode) {
  return mode == MultinodeTest::DriverMode::kAsyncDelegatedAlloc ||
         mode ==
             MultinodeTest::DriverMode::kAsyncObjectBrokeringAndDelegatedAlloc;
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

void MultinodeTest::CreateTransports(TestNodeType node0_type,
                                     TestNodeType node1_type,
                                     DriverMode mode,
                                     IpczDriverHandle* transport0,
                                     IpczDriverHandle* transport1) {
  if (mode == DriverMode::kSync) {
    // The sync driver doesn't care about context, so use it directly.
    IpczResult result = GetDriver(mode)->CreateTransports(
        IPCZ_INVALID_DRIVER_HANDLE, IPCZ_INVALID_DRIVER_HANDLE, IPCZ_NO_FLAGS,
        nullptr, transport0, transport1);
    ABSL_ASSERT(result == IPCZ_RESULT_OK);
    return;
  }

  auto [one, two] = reference_drivers::Channel::CreateChannelPair();

  // The multiprocess reference driver uses the presence of a valid remote
  // OSProcess handle as a signal that the local node is a broker. We preserve
  // this condition despite these transports always being used in-process, as it
  // allows for better coverage of driver object relaying.
  using reference_drivers::OSProcess;
  auto remote_process_for_transport = [](TestNodeType type) {
    return type == TestNodeType::kBroker ? OSProcess::GetCurrent()
                                         : OSProcess();
  };

  using Source = reference_drivers::MultiprocessTransportSource;
  auto source_for_node = [](TestNodeType type) {
    return type == TestNodeType::kBroker ? Source::kFromBroker
                                         : Source::kFromNonBroker;
  };

  using Target = reference_drivers::MultiprocessTransportTarget;
  auto target_for_remote_node = [](TestNodeType type) {
    return type == TestNodeType::kBroker ? Target::kToBroker
                                         : Target::kToNonBroker;
  };

  *transport0 = reference_drivers::CreateTransportFromChannel(
      std::move(one), remote_process_for_transport(node0_type),
      source_for_node(node0_type), target_for_remote_node(node1_type));
  *transport1 = reference_drivers::CreateTransportFromChannel(
      std::move(two), remote_process_for_transport(node1_type),
      source_for_node(node1_type), target_for_remote_node(node0_type));
}

IpczHandle MultinodeTest::ConnectToBroker(DriverMode mode,
                                          IpczHandle non_broker_node,
                                          IpczDriverHandle transport) {
  IpczHandle portal;
  IpczConnectNodeFlags flags = IPCZ_CONNECT_NODE_TO_BROKER;
  if (UseDelegatedAlloc(mode)) {
    flags |= IPCZ_CONNECT_NODE_TO_ALLOCATION_DELEGATE;
  }
  IpczResult result =
      ipcz.ConnectNode(non_broker_node, transport, 1, flags, nullptr, &portal);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
  return portal;
}

IpczHandle MultinodeTest::ConnectToNonBroker(IpczHandle broker_node,
                                             IpczDriverHandle transport) {
  IpczHandle portal;
  IpczResult result = ipcz.ConnectNode(broker_node, transport, 1, IPCZ_NO_FLAGS,
                                       nullptr, &portal);
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
  CreateTransports(TestNodeType::kBroker, TestNodeType::kNonBroker, mode,
                   &transport0, &transport1);

  *broker_portal = ConnectToNonBroker(broker_node, transport0);
  *non_broker_portal = ConnectToBroker(mode, non_broker_node, transport1);
}

}  // namespace ipcz::test
