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

  void ConnectNodes(IpczHandle node0,
                    IpczConnectNodeFlags flags0,
                    IpczHandle node1,
                    IpczConnectNodeFlags flags1,
                    IpczHandle* portal0,
                    IpczHandle* portal1) {
    IpczDriverHandle transports[2];
    CreateTransports(&transports[0], &transports[1]);
    *portal0 = ConnectNode(node0, transports[0], flags0);
    *portal1 = ConnectNode(node1, transports[1], flags1);
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
  IpczHandle node_a = CreateBrokerNode();
  IpczHandle node_b = CreateNonBrokerNode();
  IpczHandle node_c = CreateNonBrokerNode();

  IpczHandle a_to_b, b_to_a;
  ConnectNodes(node_a, IPCZ_NO_FLAGS, node_b, IPCZ_CONNECT_NODE_TO_BROKER,
               &a_to_b, &b_to_a);

  IpczHandle a_to_c, c_to_a;
  ConnectNodes(node_a, IPCZ_NO_FLAGS, node_c, IPCZ_CONNECT_NODE_TO_BROKER,
               &a_to_c, &c_to_a);

  TestNodeConnections(node_a, a_to_b, b_to_a, a_to_c, c_to_a);

  ClosePortals({a_to_b, b_to_a, a_to_c, c_to_a});
  DestroyNodes({node_a, node_b, node_c});
}

TEST_P(ConnectTest, NonBrokerToNonBrokerWithoutBroker) {
  IpczHandle node_a = CreateNonBrokerNode();
  IpczHandle node_b = CreateNonBrokerNode();

  IpczDriverHandle transports[2];
  CreateTransports(&transports[0], &transports[1]);

  IpczOSProcessHandle process = {sizeof(process)};
  os::Process::ToIpczOSProcessHandle(os::Process::GetCurrent(), process);

  IpczHandle portal;
  EXPECT_EQ(IPCZ_RESULT_FAILED_PRECONDITION,
            ipcz.ConnectNode(node_a, transports[0], &process, 1, IPCZ_NO_FLAGS,
                             nullptr, &portal));

  EXPECT_EQ(IPCZ_RESULT_FAILED_PRECONDITION,
            ipcz.ConnectNode(node_b, transports[1], &process, 1, IPCZ_NO_FLAGS,
                             nullptr, &portal));
}

INSTANTIATE_MULTINODE_TEST_SUITE_P(ConnectTest);

}  // namespace
}  // namespace ipcz
