// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/multinode_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using MergePortalsTest = test::MultinodeTestWithDriver;

TEST_P(MergePortalsTest, LocalMerge) {
  IpczHandle node = CreateBrokerNode();

  IpczHandle a, b;
  OpenPortals(node, &a, &b);

  IpczHandle c, d;
  OpenPortals(node, &c, &d);

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.MergePortals(b, c, IPCZ_NO_FLAGS, nullptr));

  Put(a, "hi", {}, {});

  Parcel p = Get(d);
  EXPECT_EQ("hi", p.message);

  Put(d, "yo", {}, {});
  p = Get(a);
  EXPECT_EQ("yo", p.message);

  EXPECT_TRUE(PortalsAreLocalPeers(a, d));

  ClosePortals({a, d});
  DestroyNodes({node});
}

TEST_P(MergePortalsTest, MergeWithConnectNodePortal) {
  IpczHandle node0 = CreateBrokerNode();
  IpczHandle node1 = CreateNonBrokerNode();

  IpczHandle a, b;
  Connect(node0, node1, &a, &b);

  IpczHandle c, d;
  OpenPortals(node1, &c, &d);

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.MergePortals(b, c, IPCZ_NO_FLAGS, nullptr));

  Put(a, "hi", {}, {});

  Parcel p;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(d, p));
  EXPECT_EQ("hi", p.message);

  Put(d, "yo", {}, {});
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
  EXPECT_EQ("yo", p.message);

  while (GetNumRouters() > 2) {
    VerifyEndToEnd(a, d);
  }

  ClosePortals({a, d});
  DestroyNodes({node0, node1});
}

TEST_P(MergePortalsTest, MergeWithExtendedRemoteRoute) {
  constexpr size_t kNumOtherNodes = 4;
  IpczHandle broker;
  IpczHandle nodes[kNumOtherNodes];
  IpczHandle broker_to_node[kNumOtherNodes];
  IpczHandle node_to_broker[kNumOtherNodes];
  broker = CreateBrokerNode();
  for (size_t i = 0; i < kNumOtherNodes; ++i) {
    nodes[i] = CreateNonBrokerNode();
    Connect(broker, nodes[i], &broker_to_node[i], &node_to_broker[i]);
  }

  IpczHandle a, b;
  OpenPortals(broker, &a, &b);

  IpczHandle c, d;
  OpenPortals(broker, &c, &d);

  // Send both `a` and `d` around the world in opposite directions before
  // returning to node 0. Then merge `b` and `c`. Everything should decay
  // normally and `a` and `d` should still end as direct local peers.
  for (size_t i = 0; i < kNumOtherNodes; ++i) {
    const size_t j = kNumOtherNodes - i - 1;
    Put(broker_to_node[i], "hi", {&a, 1}, {});
    Put(broker_to_node[j], "hi", {&d, 1}, {});

    Parcel p;
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(node_to_broker[i], p));
    ASSERT_EQ(1u, p.portals.size());
    a = p.portals[0];
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(node_to_broker[j], p));
    ASSERT_EQ(1u, p.portals.size());
    d = p.portals[0];

    Put(node_to_broker[i], "ok", {&a, 1}, {});
    Put(node_to_broker[j], "ok", {&d, 1}, {});

    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(broker_to_node[i], p));
    ASSERT_EQ(1u, p.portals.size());
    a = p.portals[0];
    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(broker_to_node[j], p));
    ASSERT_EQ(1u, p.portals.size());
    d = p.portals[0];
  }

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.MergePortals(b, c, IPCZ_NO_FLAGS, nullptr));

  while (!PortalsAreLocalPeers(a, d)) {
    VerifyEndToEnd(a, d);
  }

  ClosePortals({a, d});
  ClosePortals(broker_to_node);
  ClosePortals(node_to_broker);
  DestroyNodes(nodes);
  DestroyNodes({broker});
}

INSTANTIATE_MULTINODE_TEST_SUITE_P(MergePortalsTest);

}  // namespace
}  // namespace ipcz
