// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "os/process.h"
#include "test/api_test.h"
#include "test/test_client.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using RemotePortalTest = test::APITest;

TEST_F(RemotePortalTest, BasicConnection) {
  IpczHandle a, b;
  IpczHandle other_node = CreateSingleProcessNode();
  ConnectSingleProcessBrokerToNonBroker(node(), other_node, &a, &b);

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
  ipcz.DestroyNode(other_node, IPCZ_NO_FLAGS, nullptr);
}

TEST_F(RemotePortalTest, TransferLocalPortal) {
  IpczHandle a, b;
  IpczHandle other_node = CreateSingleProcessNode();
  ConnectSingleProcessBrokerToNonBroker(node(), other_node, &a, &b);

  IpczHandle c, d;
  OpenPortals(&c, &d);

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

  ipcz.DestroyNode(other_node, IPCZ_NO_FLAGS, nullptr);
}

TEST_F(RemotePortalTest, TransferManyLocalPortals) {
  IpczHandle a, b;
  IpczHandle other_node = CreateSingleProcessNode();
  ConnectSingleProcessBrokerToNonBroker(node(), other_node, &a, &b);

  constexpr uint32_t kNumIterations = 100;
  for (uint32_t i = 0; i < kNumIterations; ++i) {
    IpczHandle c, d;
    ipcz.OpenPortals(node(), IPCZ_NO_FLAGS, nullptr, &c, &d);
    Put(a, "", {&d, 1}, {});

    IpczHandle e, f;
    ipcz.OpenPortals(other_node, IPCZ_NO_FLAGS, nullptr, &e, &f);
    Put(b, "", {&f, 1}, {});

    Put(c, "ya", {}, {});
    Put(e, "na", {}, {});
    ipcz.ClosePortal(c, IPCZ_NO_FLAGS, nullptr);
    ipcz.ClosePortal(e, IPCZ_NO_FLAGS, nullptr);
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

    ipcz.ClosePortal(d, IPCZ_NO_FLAGS, nullptr);
    ipcz.ClosePortal(f, IPCZ_NO_FLAGS, nullptr);
  }

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr));

  ipcz.DestroyNode(other_node, IPCZ_NO_FLAGS, nullptr);
}

TEST_F(RemotePortalTest, MultipleHops) {
  IpczHandle node0 = CreateSingleProcessNode(IPCZ_CREATE_NODE_AS_BROKER);
  IpczHandle node1 = CreateSingleProcessNode();
  IpczHandle node2 = CreateSingleProcessNode();

  IpczHandle a, b;
  ConnectSingleProcessBrokerToNonBroker(node0, node1, &a, &b);

  IpczHandle c, d;
  ConnectSingleProcessBrokerToNonBroker(node0, node2, &c, &d);

  // Send `f` from node1 to node0 and then from node0 to node2
  IpczHandle e, f1;
  ipcz.OpenPortals(node1, IPCZ_NO_FLAGS, nullptr, &e, &f1);
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

  ipcz.DestroyNode(node0, IPCZ_NO_FLAGS, nullptr);
  ipcz.DestroyNode(node1, IPCZ_NO_FLAGS, nullptr);
  ipcz.DestroyNode(node2, IPCZ_NO_FLAGS, nullptr);
}

TEST_F(RemotePortalTest, SendAndCloseFromBufferingNonBroker) {
  // Covers the case where a newly connected non-broker node sends a parcel on
  // one of the initial portals and then closes the portal immediately. The test
  // verifies that the parcel is eventually delivered once the peer node
  // completes the connection process.
  IpczHandle other_node = CreateSingleProcessNode();

  IpczDriverHandle transport0, transport1;
  CreateSingleProcessTransports(&transport0, &transport1);

  IpczHandle b = ConnectToBroker(other_node, transport1);

  const std::string kMessage = "woot";
  Put(b, kMessage, {}, {});
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr));

  IpczHandle a =
      ConnectToNonBroker(node(), transport0, os::Process::GetCurrent());

  Parcel p;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(kMessage, p.message);

  ipcz.DestroyNode(other_node, IPCZ_NO_FLAGS, nullptr);
}

TEST_F(RemotePortalTest, MultipleHopsThenSendAndClose) {
  // Covers the case where a new remote portal is accepted and then it
  // immediately sends a parcel and closes itself. Verifies that in this case
  // the sent parcel is actually delivered to its destination.
  IpczHandle node1 = CreateSingleProcessNode();
  IpczHandle node2 = CreateSingleProcessNode();

  IpczHandle a, b;
  ConnectSingleProcessBrokerToNonBroker(node(), node1, &a, &b);

  IpczHandle c, d;
  ConnectSingleProcessBrokerToNonBroker(node(), node2, &c, &d);

  IpczHandle e, f1;
  ipcz.OpenPortals(node1, IPCZ_NO_FLAGS, nullptr, &e, &f1);
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
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(f2, IPCZ_NO_FLAGS, nullptr));

  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(e, p));
  EXPECT_EQ(kMessage, p.message);

  ClosePortals({a, b, c, d, e});

  ipcz.DestroyNode(node1, IPCZ_NO_FLAGS, nullptr);
  ipcz.DestroyNode(node2, IPCZ_NO_FLAGS, nullptr);
}

const std::string kMultiprocessMessageFromA = "hello!";
const std::string kMultiprocessMessageFromB = "hey hey";

TEST_F(RemotePortalTest, BasicMultiprocess) {
  test::TestClient client("BasicMultiprocessClient");
  IpczHandle node = CreateMultiprocessNode(IPCZ_CREATE_NODE_AS_BROKER);
  IpczHandle a = ConnectToNonBroker(
      node, CreateMultiprocessTransport(client.channel()), client.process());

  Put(a, kMultiprocessMessageFromA, {}, {});

  Parcel p;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
  EXPECT_EQ(kMultiprocessMessageFromB, p.message);

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.DestroyNode(node, IPCZ_NO_FLAGS, nullptr));
}

TEST_CLIENT_F(RemotePortalTest, BasicMultiprocessClient, c) {
  IpczHandle node = CreateMultiprocessNode();
  IpczHandle b = ConnectToBroker(node, CreateMultiprocessTransport(c));
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
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.DestroyNode(node, IPCZ_NO_FLAGS, nullptr));
}

}  // namespace
}  // namespace ipcz
