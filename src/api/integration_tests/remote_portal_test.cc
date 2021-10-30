// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "debug/log.h"
#include "ipcz/ipcz.h"
#include "os/channel.h"
#include "os/process.h"
#include "test/api_test.h"
#include "test/test_client.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using RemotePortalTest = test::APITest;

TEST_F(RemotePortalTest, BasicConnection) {
  IpczHandle other_node;
  ASSERT_EQ(IPCZ_RESULT_OK,
            ipcz.CreateNode(IPCZ_NO_FLAGS, nullptr, &other_node));

  os::Channel local, remote;
  std::tie(local, remote) = os::Channel::CreateChannelPair();

  os::Channel::OSTransportWithHandle local_transport;
  os::Channel::ToOSTransport(std::move(local), local_transport);

  IpczOSProcessHandle process = {sizeof(process)};
  os::Process::ToIpczOSProcessHandle(os::Process::GetCurrent(), process);

  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenRemotePortal(node(), &local_transport, &process,
                                  IPCZ_NO_FLAGS, nullptr, &a));

  os::Channel::OSTransportWithHandle remote_transport;
  os::Channel::ToOSTransport(std::move(remote), remote_transport);

  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.AcceptRemotePortal(other_node, &remote_transport,
                                    IPCZ_NO_FLAGS, nullptr, &b));

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
  IpczHandle other_node;
  ASSERT_EQ(IPCZ_RESULT_OK,
            ipcz.CreateNode(IPCZ_NO_FLAGS, nullptr, &other_node));

  os::Channel local, remote;
  std::tie(local, remote) = os::Channel::CreateChannelPair();

  os::Channel::OSTransportWithHandle local_transport;
  os::Channel::ToOSTransport(std::move(local), local_transport);

  IpczOSProcessHandle process = {sizeof(process)};
  os::Process::ToIpczOSProcessHandle(os::Process::GetCurrent(), process);

  IpczHandle a, b;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenRemotePortal(node(), &local_transport, &process,
                                  IPCZ_NO_FLAGS, nullptr, &a));

  IpczHandle c, d;
  OpenPortals(&c, &d);

  os::Channel::OSTransportWithHandle remote_transport;
  os::Channel::ToOSTransport(std::move(remote), remote_transport);

  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.AcceptRemotePortal(other_node, &remote_transport,
                                    IPCZ_NO_FLAGS, nullptr, &b));

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

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr));
  EXPECT_EQ(IPCZ_RESULT_NOT_FOUND, WaitToGet(b, b_parcel));

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr));

  Parcel d_parcel;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(d, d_parcel));
  EXPECT_EQ(kMessageFromC, d_parcel.message);

  ipcz.DestroyNode(other_node, IPCZ_NO_FLAGS, nullptr);
}

const std::string kMultiprocessMessageFromA = "hello!";
const std::string kMultiprocessMessageFromB = "hey hey";

TEST_F(RemotePortalTest, BasicMultiprocess) {
  test::TestClient client("BasicMultiprocessClient");
  IpczHandle a = OpenRemotePortal(client);

  Put(a, kMultiprocessMessageFromA, {}, {});

  Parcel p;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
  EXPECT_EQ(kMultiprocessMessageFromB, p.message);

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr));
}

TEST_CLIENT_F(RemotePortalTest, BasicMultiprocessClient, c) {
  IpczHandle b = AcceptRemotePortal(c);
  Put(b, kMultiprocessMessageFromB, {}, {});

  Parcel p;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, p));
  EXPECT_EQ(kMultiprocessMessageFromA, p.message);

  EXPECT_EQ(IPCZ_RESULT_NOT_FOUND, WaitToGet(b, p));

  IpczPortalStatus status = {sizeof(status)};
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.QueryPortalStatus(b, IPCZ_NO_FLAGS, nullptr, &status));
  EXPECT_EQ(IPCZ_PORTAL_STATUS_PEER_CLOSED, status.flags);
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr));
}
}  // namespace
}  // namespace ipcz
