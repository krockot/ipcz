// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/multinode_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

class ConnectTest : public test::MultinodeTestWithDriver {
 public:
  IpczHandle ConnectNode(IpczHandle node,
                         IpczDriverHandle transport,
                         IpczConnectNodeFlags flags) {
    IpczOSProcessHandle process = {sizeof(process)};
    os::Process::ToIpczOSProcessHandle(os::Process::GetCurrent(), process);

    IpczHandle portal;
    EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ConnectNode(node, transport, &process, 1,
                                               flags, nullptr, &portal));
    return portal;
  }

  void TestNodeConnections(IpczHandle node0,
                           IpczHandle node0_to_node1,
                           IpczHandle node1_to_node0,
                           IpczHandle node0_to_node2,
                           IpczHandle node2_to_node0) {
    // Starting on one node and moving each portal to a separate other node
    // guarantees that one node will need to be introduced to the other. This
    // serves as a good test of whether connections are established correctly
    // and consistently.
    IpczHandle q, p;
    OpenPortals(node0, &q, &p);
    Put(node0_to_node1, "hey", {&q, 1});
    Put(node0_to_node2, "hi", {&p, 1});

    Parcel parcel;
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(node1_to_node0, parcel));
    ASSERT_EQ(1u, parcel.portals.size());
    q = parcel.portals[0];
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(node2_to_node0, parcel));
    ASSERT_EQ(1u, parcel.portals.size());
    p = parcel.portals[0];

    while (GetNumRouters() > 6) {
      VerifyEndToEnd(q, p);
    }

    ClosePortals({q, p});
  }
};

TEST_P(ConnectTest, BrokerToNonBroker) {
  IpczHandle broker = CreateBrokerNode();
  IpczHandle node_a = CreateNonBrokerNode();
  IpczHandle node_b = CreateNonBrokerNode();

  IpczDriverHandle transport_a[2];
  CreateTransports(&transport_a[0], &transport_a[1]);
  IpczHandle broker_to_a = ConnectNode(broker, transport_a[0], IPCZ_NO_FLAGS);
  IpczHandle a_to_broker =
      ConnectNode(node_a, transport_a[1], IPCZ_CONNECT_NODE_TO_BROKER);

  IpczDriverHandle transport_b[2];
  CreateTransports(&transport_b[0], &transport_b[1]);
  IpczHandle broker_to_b = ConnectNode(broker, transport_b[0], IPCZ_NO_FLAGS);
  IpczHandle b_to_broker =
      ConnectNode(node_b, transport_b[1], IPCZ_CONNECT_NODE_TO_BROKER);

  TestNodeConnections(broker, broker_to_a, a_to_broker, broker_to_b,
                      b_to_broker);

  ClosePortals({broker_to_a, a_to_broker, broker_to_b, b_to_broker});
  DestroyNodes({broker, node_a, node_b});
}

INSTANTIATE_MULTINODE_TEST_SUITE_P(ConnectTest);

}  // namespace
}  // namespace ipcz
