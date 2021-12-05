// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "os/process.h"
#include "test/multinode_test.h"
#include "test/multiprocess_test.h"
#include "test/test_client.h"
#include "testing/gtest/include/gtest/gtest.h"

#include <chrono>
#include <thread>

namespace ipcz {
namespace {

using RemotePortalTest = test::MultinodeTestWithDriver;

TEST_P(RemotePortalTest, BasicConnection) {
  IpczHandle a, b;
  IpczHandle node = CreateBrokerNode();
  IpczHandle other_node = CreateNonBrokerNode();
  Connect(node, other_node, &a, &b);

  const std::string kMessageFromA = "hello!";
  const std::string kMessageFromB = "hey hey";
  Put(a, kMessageFromA, {}, {});
  Put(b, kMessageFromB, {}, {});

  Parcel a_parcel;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, a_parcel));
  EXPECT_EQ(kMessageFromB, a_parcel.message);

  Parcel b_parcel;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, b_parcel));
  EXPECT_EQ(kMessageFromA, b_parcel.message);

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_NOT_FOUND, WaitToGet(b, b_parcel));

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr));
  DestroyNodes({node, other_node});
}

TEST_P(RemotePortalTest, TransferLocalPortal) {
  IpczHandle a, b;
  IpczHandle node = CreateBrokerNode();
  IpczHandle other_node = CreateNonBrokerNode();
  Connect(node, other_node, &a, &b);

  IpczHandle c, d;
  OpenPortals(node, &c, &d);

  const std::string kMessageFromA = "hello!";
  const std::string kMessageFromB = "hey hey";
  const std::string kMessageFromC = "drink slurm";
  Put(a, kMessageFromA, {&d, 1}, {});
  Put(b, kMessageFromB, {}, {});
  Put(c, kMessageFromC, {}, {});

  Parcel a_parcel;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, a_parcel));
  EXPECT_EQ(kMessageFromB, a_parcel.message);

  Parcel b_parcel;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, b_parcel));
  EXPECT_EQ(kMessageFromA, b_parcel.message);
  ASSERT_EQ(1u, b_parcel.portals.size());
  d = b_parcel.portals[0];

  ClosePortals({a, b});

  Parcel d_parcel;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(d, d_parcel));
  EXPECT_EQ(kMessageFromC, d_parcel.message);

  ClosePortals({c, d});
  DestroyNodes({node, other_node});
}

TEST_P(RemotePortalTest, TransferManyLocalPortals) {
  IpczHandle a, b;
  IpczHandle node = CreateBrokerNode();
  IpczHandle other_node = CreateNonBrokerNode();
  Connect(node, other_node, &a, &b);

  constexpr uint32_t kNumIterations = 100;
  for (uint32_t i = 0; i < kNumIterations; ++i) {
    IpczHandle c, d;
    OpenPortals(node, &c, &d);
    Put(a, "", {&d, 1}, {});

    IpczHandle e, f;
    OpenPortals(other_node, &e, &f);
    Put(b, "", {&f, 1}, {});

    Put(c, "ya", {}, {});
    Put(e, "na", {}, {});
    ClosePortals({c, e});
  }

  for (uint32_t i = 0; i < kNumIterations; ++i) {
    Parcel p;
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
    ASSERT_EQ(1u, p.portals.size());
    IpczHandle f = p.portals[0];
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(f, p));
    EXPECT_EQ("na", p.message);

    Parcel q;
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, q));
    ASSERT_EQ(1u, q.portals.size());
    IpczHandle d = q.portals[0];
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(d, q));
    EXPECT_EQ("ya", q.message);

    ClosePortals({d, f});
  }

  ClosePortals({a, b});
  DestroyNodes({node, other_node});
}

TEST_P(RemotePortalTest, MultipleHops) {
  IpczHandle node0 = CreateBrokerNode();
  IpczHandle node1 = CreateNonBrokerNode();
  IpczHandle node2 = CreateNonBrokerNode();

  IpczHandle a, b;
  Connect(node0, node1, &a, &b);

  IpczHandle c, d;
  Connect(node0, node2, &c, &d);

  // Send `f` from node1 to node0 and then from node0 to node2
  IpczHandle e, f1;
  OpenPortals(node1, &e, &f1);
  Put(b, "here", {&f1, 1}, {});
  Parcel p;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
  ASSERT_EQ(1u, p.portals.size());
  IpczHandle f0 = p.portals[0];
  Put(c, "ok ok", {&f0, 1}, {});
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(d, p));
  ASSERT_EQ(1u, p.portals.size());
  IpczHandle f2 = p.portals[0];

  constexpr uint32_t kNumIterations = 100;
  for (uint32_t i = 0; i < kNumIterations; ++i) {
    Put(e, "merp", {}, {});
    Put(f2, "nerp", {}, {});
  }
  for (uint32_t i = 0; i < kNumIterations; ++i) {
    Parcel p;
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(f2, p));
    EXPECT_EQ("merp", p.message);
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(e, p));
    EXPECT_EQ("nerp", p.message);
  }

  ClosePortals({a, b, c, d, e, f2});
  DestroyNodes({node0, node1, node2});
}

TEST_P(RemotePortalTest, SendAndCloseFromBufferingNonBroker) {
  // Covers the case where a newly connected non-broker node sends a parcel on
  // one of the initial portals and then closes the portal immediately. The test
  // verifies that the parcel is eventually delivered once the peer node
  // completes the connection process.

  IpczHandle node = CreateBrokerNode();
  IpczHandle other_node = CreateNonBrokerNode();

  IpczDriverHandle transport0, transport1;
  CreateTransports(&transport0, &transport1);

  IpczHandle b = ConnectToBroker(other_node, transport1);

  const std::string kMessage = "woot";
  Put(b, kMessage, {}, {});
  ClosePortals({b});

  IpczHandle a = ConnectToNonBroker(node, transport0);

  Parcel p;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
  ClosePortals({a});
  EXPECT_EQ(kMessage, p.message);

  DestroyNodes({node, other_node});
}

TEST_P(RemotePortalTest, MultipleHopsThenSendAndClose) {
  // Covers the case where a new remote portal is accepted and then it
  // immediately sends a parcel and closes itself. Verifies that in this case
  // the sent parcel is actually delivered to its destination.

  IpczHandle node0 = CreateBrokerNode();
  IpczHandle node1 = CreateNonBrokerNode();
  IpczHandle node2 = CreateNonBrokerNode();

  IpczHandle a, b;
  Connect(node0, node1, &a, &b);

  IpczHandle c, d;
  Connect(node0, node2, &c, &d);

  IpczHandle e, f1;
  OpenPortals(node1, &e, &f1);
  Put(b, "here", {&f1, 1}, {});
  Parcel p;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
  ASSERT_EQ(1u, p.portals.size());
  IpczHandle f0 = p.portals[0];
  Put(c, "ok ok", {&f0, 1}, {});
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(d, p));
  ASSERT_EQ(1u, p.portals.size());
  IpczHandle f2 = p.portals[0];

  const std::string kMessage = "woot";
  Put(f2, kMessage, {}, {});
  ClosePortals({f2});

  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(e, p));
  EXPECT_EQ(kMessage, p.message);

  ClosePortals({a, b, c, d, e});
  DestroyNodes({node0, node1, node2});
}

TEST_P(RemotePortalTest, TransferBackAndForth) {
  IpczHandle node = CreateBrokerNode();
  IpczHandle other_node = CreateNonBrokerNode();

  IpczHandle a, b;
  Connect(node, other_node, &a, &b);

  IpczHandle c, d;
  OpenPortals(node, &c, &d);

  Parcel p;
  constexpr size_t kNumIterations = 8;
  for (size_t i = 0; i < kNumIterations; ++i) {
    Put(c, "hi", {}, {});
    Put(a, "", {&d, 1}, {});
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, p));
    ASSERT_EQ(1u, p.portals.size());
    d = p.portals[0];
    Put(b, "", {&d, 1}, {});
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
    ASSERT_EQ(1u, p.portals.size());
    d = p.portals[0];
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(d, p));
    EXPECT_EQ("hi", p.message);
  }

  size_t count = 0;
  while (!PortalsAreLocalPeers(c, d)) {
    ++count;
    if (count == 10) {
      using namespace std::chrono_literals;
      std::this_thread::sleep_for(1s);
      LogPortalRoute(c);
      std::this_thread::sleep_for(1s);
    }
    VerifyEndToEnd(c, d);
  }
  VerifyEndToEnd(c, d);

  ClosePortals({a, b, c, d});
  DestroyNodes({node, other_node});
}

TEST_P(RemotePortalTest, ExpansionInBothDirections) {
  constexpr size_t kNumHops = 4;
  IpczHandle left_nodes[kNumHops];
  IpczHandle right_nodes[kNumHops];

  IpczHandle to_left[kNumHops];
  IpczHandle from_left[kNumHops];
  IpczHandle to_right[kNumHops];
  IpczHandle from_right[kNumHops];

  IpczHandle broker = CreateBrokerNode();
  for (size_t i = 0; i < kNumHops; ++i) {
    left_nodes[i] = CreateNonBrokerNode();
    Connect(broker, left_nodes[i], &to_left[i], &from_left[i]);
    right_nodes[i] = CreateNonBrokerNode();
    Connect(broker, right_nodes[i], &to_right[i], &from_right[i]);
  }

  IpczHandle a, b;
  OpenPortals(broker, &a, &b);
  for (size_t i = 0; i < kNumHops; ++i) {
    Put(to_left[i], "", {&a, 1}, {});
    Put(to_right[i], "", {&b, 1}, {});

    Parcel p;
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(from_left[i], p));
    ASSERT_EQ(1u, p.portals.size());
    a = p.portals[0];

    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(from_right[i], p));
    ASSERT_EQ(1u, p.portals.size());
    b = p.portals[0];

    Put(from_left[i], "", {&a, 1}, {});
    Put(from_right[i], "", {&b, 1}, {});

    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(to_left[i], p));
    ASSERT_EQ(1u, p.portals.size());
    a = p.portals[0];

    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(to_right[i], p));
    ASSERT_EQ(1u, p.portals.size());
    b = p.portals[0];
  }

  while (!PortalsAreLocalPeers(a, b)) {
    VerifyEndToEnd(a, b);
  }
  VerifyEndToEnd(a, b);

  ClosePortals({a, b});
  for (size_t i = 0; i < kNumHops; ++i) {
    ClosePortals({to_left[i], from_left[i], to_right[i], from_right[i]});
  }
  for (size_t i = 0; i < kNumHops; ++i) {
    DestroyNodes({left_nodes[i], right_nodes[i]});
  }
  DestroyNodes({broker});
}

TEST_P(RemotePortalTest, LocalProxyBypass) {
  // Tests an edge case where bypassing a proxy results in the creation of a
  // local proxying link between routers on the same node, and further route
  // contraction requires this link to decay in turn. Getting into this scenario
  // is racy, so the test will not always cover the intended path.

  // The idea here is to start with a portal pair between two nodes. We send
  // each end off to two different nodes, from which they're both immediately
  // sent back to the original node. This can fairly often land us in a
  // configuration like:
  //
  //   A   B   C
  //
  //       L2
  //     /
  //   L1--L0--R1
  //          /
  //       R2
  //
  // Where columns A, B, and C represent distinct nodes. If L0 is chosen to
  // decay first, the scenario is not interesting yet. But if R1 is chosen
  // first, node B will end up with a local peer link between L0 and R2:
  //
  //      L2
  //    /
  //  L1--L0
  //       |
  //      R2
  //
  // This link itself then requires special care to decay, and L0 will directly
  // provide L1 with a new route to R2 and tell it to bypass L0. This will
  // finally result in:
  //
  //      L2
  //    /
  //   L1 . . . L0
  //    \     .
  //      R2 .
  //
  // And once L0 fully decays, L1 will initiate its own bypass resulting in the
  // direct link between L2 and R2 on the same node B.

  IpczHandle node_0 = CreateNonBrokerNode();
  IpczHandle node_1 = CreateBrokerNode();
  IpczHandle node_2 = CreateNonBrokerNode();

  IpczHandle a, b;
  Connect(node_1, node_0, &a, &b);

  IpczHandle c, d;
  Connect(node_1, node_2, &c, &d);

  IpczHandle q, p;
  OpenPortals(node_1, &q, &p);

  Put(a, "", {&q, 1}, {});
  Put(c, "", {&p, 1}, {});

  Parcel parcel;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, parcel));
  ASSERT_EQ(1u, parcel.portals.size());
  q = parcel.portals[0];
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(d, parcel));
  ASSERT_EQ(1u, parcel.portals.size());
  p = parcel.portals[0];

  Put(b, "", {&q, 1}, {});
  Put(d, "", {&p, 1}, {});

  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, parcel));
  ASSERT_EQ(1u, parcel.portals.size());
  q = parcel.portals[0];
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(c, parcel));
  ASSERT_EQ(1u, parcel.portals.size());
  p = parcel.portals[0];

  Put(q, "hello");
  Put(p, "goodbye");
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(q, parcel));
  EXPECT_EQ("goodbye", parcel.message);
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(p, parcel));
  EXPECT_EQ("hello", parcel.message);

  ClosePortals({q, p, a, b, c, d});
  DestroyNodes({node_0, node_1, node_2});
}

using MultiprocessRemotePortalTest = test::MultiprocessTest;

const std::string kMultiprocessMessageFromA = "hello!";
const std::string kMultiprocessMessageFromB = "hey hey";

TEST_F(MultiprocessRemotePortalTest, BasicMultiprocess) {
  test::TestClient client("BasicMultiprocessClient");
  IpczHandle a = ConnectToClient(client);

  Put(a, kMultiprocessMessageFromA, {}, {});

  Parcel p;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
  EXPECT_EQ(kMultiprocessMessageFromB, p.message);

  ClosePortals({a});
}

TEST_CLIENT_F(MultiprocessRemotePortalTest, BasicMultiprocessClient, c) {
  IpczHandle b = ConnectToBroker(c);
  Put(b, kMultiprocessMessageFromB, {}, {});

  Parcel p;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, p));
  EXPECT_EQ(kMultiprocessMessageFromA, p.message);

  EXPECT_EQ(IPCZ_RESULT_NOT_FOUND, WaitToGet(b, p));

  IpczPortalStatus status = {sizeof(status)};
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.QueryPortalStatus(b, IPCZ_NO_FLAGS, nullptr, &status));
  EXPECT_EQ(IPCZ_PORTAL_STATUS_PEER_CLOSED | IPCZ_PORTAL_STATUS_DEAD,
            status.flags);
  ClosePortals({b});
}

INSTANTIATE_MULTINODE_TEST_SUITE_P(RemotePortalTest);

}  // namespace
}  // namespace ipcz
