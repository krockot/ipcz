// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "build/build_config.h"
#include "ipcz/ipcz.h"
#include "test/multinode_test.h"
#include "test/multiprocess_test.h"
#include "test/test_client.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "util/os_process.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

namespace ipcz {
namespace {

using RemotePortalTest = test::MultinodeTestWithDriver;

TEST_P(RemotePortalTest, BasicConnection) {
  IpczHandle a, b;
  IpczHandle node = CreateBrokerNode();
  IpczHandle other_node = CreateNonBrokerNode();
  Connect(node, other_node, &a, &b);

  VerifyEndToEnd(a, b);
  EXPECT_EQ(2u, GetNumRouters());

  ClosePortals({a, b});
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
  Put(a, kMessageFromA, {&d, 1});
  Put(b, kMessageFromB);
  Put(c, kMessageFromC);

  Parcel a_parcel;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, a_parcel));
  EXPECT_EQ(kMessageFromB, a_parcel.message);

  Parcel b_parcel;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, b_parcel));
  EXPECT_EQ(kMessageFromA, b_parcel.message);
  ASSERT_EQ(1u, b_parcel.handles.size());
  d = b_parcel.handles[0];

  ClosePortals({a, b});

  Parcel d_parcel;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(d, d_parcel));
  EXPECT_EQ(kMessageFromC, d_parcel.message);

  while (GetNumRouters() > 2) {
    VerifyEndToEnd(c, d);
  }

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
    Put(a, "", {&d, 1});

    IpczHandle e, f;
    OpenPortals(other_node, &e, &f);
    Put(b, "", {&f, 1});

    Put(c, "ya");
    Put(e, "na");
    ClosePortals({c, e});
  }

  for (uint32_t i = 0; i < kNumIterations; ++i) {
    Parcel p;
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
    ASSERT_EQ(1u, p.handles.size());
    IpczHandle f = p.handles[0];
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(f, p));
    EXPECT_EQ("na", p.message);

    Parcel q;
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, q));
    ASSERT_EQ(1u, q.handles.size());
    IpczHandle d = q.handles[0];
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(d, q));
    EXPECT_EQ("ya", q.message);

    ClosePortals({d, f});
  }

  while (GetNumRouters() > 2) {
    VerifyEndToEnd(a, b);
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
  Put(b, "here", {&f1, 1});
  Parcel p;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
  ASSERT_EQ(1u, p.handles.size());
  IpczHandle f0 = p.handles[0];
  Put(c, "ok ok", {&f0, 1});
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(d, p));
  ASSERT_EQ(1u, p.handles.size());
  IpczHandle f2 = p.handles[0];

  constexpr uint32_t kNumIterations = 100;
  for (uint32_t i = 0; i < kNumIterations; ++i) {
    Put(e, "merp");
    Put(f2, "nerp");
  }
  for (uint32_t i = 0; i < kNumIterations; ++i) {
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(f2, p));
    EXPECT_EQ("merp", p.message);
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(e, p));
    EXPECT_EQ("nerp", p.message);
  }

  ClosePortals({a, b, c, d});

  while (GetNumRouters() > 2) {
    VerifyEndToEnd(e, f2);
  }

  ClosePortals({e, f2});
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
  Put(b, kMessage);
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
  Put(b, "here", {&f1, 1});
  Parcel p;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
  ASSERT_EQ(1u, p.handles.size());
  IpczHandle f0 = p.handles[0];
  Put(c, "ok ok", {&f0, 1});
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(d, p));
  ASSERT_EQ(1u, p.handles.size());
  IpczHandle f2 = p.handles[0];

  const std::string kMessage = "woot";
  Put(f2, kMessage);
  ClosePortals({f2});

  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(e, p));
  EXPECT_EQ(kMessage, p.message);

  while (GetNumRouters() > 6) {
    VerifyEndToEnd(a, b);
  }

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
    Put(c, "hi");
    Put(a, "", {&d, 1});
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, p));
    ASSERT_EQ(1u, p.handles.size());
    d = p.handles[0];
    Put(b, "", {&d, 1});
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
    ASSERT_EQ(1u, p.handles.size());
    d = p.handles[0];
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(d, p));
    EXPECT_EQ("hi", p.message);
  }

  while (GetNumRouters() > 4) {
    VerifyEndToEnd(c, d);
  }

  EXPECT_TRUE(PortalsAreLocalPeers(c, d));

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
    Put(to_left[i], "", {&a, 1});
    Put(to_right[i], "", {&b, 1});

    Parcel p;
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(from_left[i], p));
    ASSERT_EQ(1u, p.handles.size());
    a = p.handles[0];

    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(from_right[i], p));
    ASSERT_EQ(1u, p.handles.size());
    b = p.handles[0];

    Put(from_left[i], "", {&a, 1});
    Put(from_right[i], "", {&b, 1});

    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(to_left[i], p));
    ASSERT_EQ(1u, p.handles.size());
    a = p.handles[0];

    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(to_right[i], p));
    ASSERT_EQ(1u, p.handles.size());
    b = p.handles[0];
  }

  while (GetNumRouters() > (2 + kNumHops * 4)) {
    VerifyEndToEnd(a, b);
  }

  EXPECT_TRUE(PortalsAreLocalPeers(a, b));

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

  Put(a, "", {&q, 1});
  Put(c, "", {&p, 1});

  Parcel parcel;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, parcel));
  ASSERT_EQ(1u, parcel.handles.size());
  q = parcel.handles[0];
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(d, parcel));
  ASSERT_EQ(1u, parcel.handles.size());
  p = parcel.handles[0];

  Put(b, "", {&q, 1});
  Put(d, "", {&p, 1});

  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, parcel));
  ASSERT_EQ(1u, parcel.handles.size());
  q = parcel.handles[0];
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(c, parcel));
  ASSERT_EQ(1u, parcel.handles.size());
  p = parcel.handles[0];

  Put(q, "hello");
  Put(p, "goodbye");
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(q, parcel));
  EXPECT_EQ("goodbye", parcel.message);
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(p, parcel));
  EXPECT_EQ("hello", parcel.message);

  ClosePortals({a, b, c, d});

  while (GetNumRouters() > 2) {
    VerifyEndToEnd(q, p);
  }

  EXPECT_TRUE(PortalsAreLocalPeers(q, p));

  ClosePortals({q, p});
  DestroyNodes({node_0, node_1, node_2});
}

TEST_P(RemotePortalTest, SendPortalPair) {
  // An edge case of questionable value, ensure it's OK to send both portals of
  // a local pair in the same parcel to another node.

  IpczHandle node = CreateBrokerNode();
  IpczHandle other_node = CreateNonBrokerNode();

  IpczHandle a, b;
  Connect(node, other_node, &a, &b);

  IpczHandle pair[2];
  OpenPortals(node, &pair[0], &pair[1]);

  const std::string kMessage = "hiiiii";

  Put(a, kMessage, {pair, 2});

  Parcel p;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, p));
  EXPECT_EQ(kMessage, p.message);
  ASSERT_EQ(2u, p.handles.size());

  IpczHandle c = p.handles[0];
  IpczHandle d = p.handles[1];

  ClosePortals({a, b});

  while (GetNumRouters() > 2) {
    VerifyEndToEnd(c, d);
  }

  EXPECT_TRUE(PortalsAreLocalPeers(c, d));

  ClosePortals({c, d});
  DestroyNodes({node, other_node});
}

using MultiprocessRemotePortalTest = test::MultiprocessTest;

const std::string kMultiprocessMessageFromA = "hello!";
const std::string kMultiprocessMessageFromB = "hey hey";

TEST_F(MultiprocessRemotePortalTest, BasicMultiprocess) {
  test::TestClient client("BasicMultiprocessClient");
  IpczHandle a = ConnectToClient(client);

  Put(a, kMultiprocessMessageFromA);

  Parcel p;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
  EXPECT_EQ(kMultiprocessMessageFromB, p.message);

  ClosePortals({a});
}

TEST_CLIENT_F(MultiprocessRemotePortalTest, BasicMultiprocessClient, c) {
  IpczHandle b = ConnectToBroker(c);
  Put(b, kMultiprocessMessageFromB);

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

#ifdef NDEBUG
constexpr size_t kNumThroughputIterations = 1000;
#else
constexpr size_t kNumThroughputIterations = 50;
#endif

constexpr size_t kNumThroughputTestMessages = 50;

TEST_F(MultiprocessRemotePortalTest, PingPong) {
  test::TestClient client("PingPongClient");
  IpczHandle a = ConnectToClient(client);

  Parcel p;
  Put(a, "hi");
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));

  for (size_t j = 0; j < kNumThroughputIterations; ++j) {
    for (size_t i = 0; i < kNumThroughputTestMessages; ++i) {
      Put(a, kMultiprocessMessageFromA);
    }
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
    EXPECT_EQ(kMultiprocessMessageFromB, p.message);
    for (size_t i = 1; i < kNumThroughputTestMessages; ++i) {
      EXPECT_TRUE(DiscardNextParcel(a));
    }
  }

  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
  EXPECT_EQ(kMultiprocessMessageFromB, p.message);

  ClosePortals({a});
}

TEST_CLIENT_F(MultiprocessRemotePortalTest, PingPongClient, c) {
  IpczHandle b = ConnectToBroker(c);

  Parcel p;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, p));
  Put(b, "hi");

  for (size_t j = 0; j < kNumThroughputIterations; ++j) {
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, p));
    EXPECT_EQ(kMultiprocessMessageFromA, p.message);
    for (size_t i = 1; i < kNumThroughputTestMessages; ++i) {
      EXPECT_TRUE(DiscardNextParcel(b));
    }
    for (size_t i = 0; i < kNumThroughputTestMessages; ++i) {
      Put(b, kMultiprocessMessageFromB);
    }
  }

  Put(b, kMultiprocessMessageFromB);

  EXPECT_EQ(IPCZ_RESULT_NOT_FOUND, WaitToGet(b, p));
  IpczPortalStatus status = {sizeof(status)};
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.QueryPortalStatus(b, IPCZ_NO_FLAGS, nullptr, &status));
  EXPECT_EQ(IPCZ_PORTAL_STATUS_PEER_CLOSED | IPCZ_PORTAL_STATUS_DEAD,
            status.flags);
  ClosePortals({b});
}

TEST_F(MultiprocessRemotePortalTest, Disconnect) {
  test::TestClient client("DisconnectClient");
  IpczHandle a = ConnectToClient(client);

  Parcel p;
  Put(a, "hi");
  EXPECT_EQ(IPCZ_RESULT_NOT_FOUND, WaitToGet(a, p));

  ClosePortals({a});
}

TEST_CLIENT_F(MultiprocessRemotePortalTest, DisconnectClient, c) {
  IpczHandle b = ConnectToBroker(c);

  Parcel p;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, p));
  EXPECT_EQ("hi", p.message);

  // Bye!
#if BUILDFLAG(IS_WIN)
  ::TerminateProcess(::GetCurrentProcess(), 0);
#else
  _exit(0);
#endif
}
INSTANTIATE_MULTINODE_TEST_SUITE_P(RemotePortalTest);

}  // namespace
}  // namespace ipcz
