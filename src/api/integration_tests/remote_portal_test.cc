// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "debug/log.h"
#include "ipcz/ipcz.h"
#include "os/channel.h"
#include "os/process.h"
#include "test/api_test.h"
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

  ipcz.ClosePortal(a, IPCZ_NO_FLAGS, nullptr);
  ipcz.ClosePortal(b, IPCZ_NO_FLAGS, nullptr);
  ipcz.DestroyNode(other_node, IPCZ_NO_FLAGS, nullptr);
}

}  // namespace
}  // namespace ipcz
