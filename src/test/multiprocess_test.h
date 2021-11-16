// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_TEST_MULTIPROCESS_TEST_H_
#define IPCZ_SRC_TEST_MULTIPROCESS_TEST_H_

#include "test/test_base.h"
#include "test/test_client.h"

namespace ipcz {
namespace test {

// Base test fixture for true multiprocess tests. These tests can use one or
// more TestClient instances to launch subprocesses and communicate with them
// over ipcz.
//
// Multiprocess tests should be used primarily to exercise behavior which may
// be unique to multiprocess environments, such as transfer of OS handles
// between certain types of processes; and for performance tests.
//
// Because the ipcz implementation abstracts concrete I/O out to a driver API,
// it's almost always sufficient to test multinode behavior within the same
// single process using only the MultinodeTest fixture. Single-process tests
// have the advantage of being simpler to trace and debug.
class MultiprocessTest : public TestBase {
 public:
  MultiprocessTest();
  ~MultiprocessTest() override;

  IpczHandle node;

  IpczHandle ConnectToClient(TestClient& client);
  IpczHandle ConnectToBroker(os::Channel& channel);
};

}  // namespace test
}  // namespace ipcz

#endif  // IPCZ_SRC_TEST_MULTIPROCESS_TEST_H_
