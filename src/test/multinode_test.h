// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_TEST_MULTINODE_TEST_H_
#define IPCZ_SRC_TEST_MULTINODE_TEST_H_

#include "ipcz/ipcz.h"
#include "test/test_base.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace test {

// Base test fixture to support tests which exercise behavior across multiple
// ipcz nodes. These may be single-process on a synchronous driver,
// single-process on an asynchronous (e.g. multiprocess) driver, or fully
// multiprocess.
//
// This fixture mostly provides convenience methods for creating and connecting
// nodes in various useful configurations.
class MultinodeTest : public TestBase {
 public:
  // Selects which driver a new node will use. Interconnecting nodes must use
  // the same driver.
  enum class DriverMode {
    kSync,
    kAsync,
    kAsyncDelegatedAlloc,
  };

  MultinodeTest();
  ~MultinodeTest() override;

  IpczHandle CreateBrokerNode(DriverMode mode);
  IpczHandle CreateNonBrokerNode(DriverMode mode);

  void CreateTransports(DriverMode mode,
                        IpczDriverHandle* transport0,
                        IpczDriverHandle* transport1);

  IpczHandle ConnectToBroker(DriverMode mode,
                             IpczHandle non_broker_node,
                             IpczDriverHandle transport);
  IpczHandle ConnectToNonBroker(IpczHandle broker_node,
                                IpczDriverHandle transport);

  void Connect(IpczHandle broker_node,
               IpczHandle non_broker_node,
               DriverMode mode,
               IpczHandle* broker_portal,
               IpczHandle* non_broker_portal);
};

// Helper for a MultinodeTest parameterized over DriverMode. Most integration
// tests should use this for parameterization. Incongruity between synchronous
// and asynchronous test failures may indicate race conditions within ipcz, but
// if a test is failing it should usually fail in both modes, with the
// synchronous version likely being easier to debug (single-threaded, easy to
// follow stack traces, etc).
class MultinodeTestWithDriver
    : public MultinodeTest,
      public testing::WithParamInterface<MultinodeTest::DriverMode> {
 public:
  IpczHandle CreateBrokerNode() {
    return MultinodeTest::CreateBrokerNode(GetParam());
  }

  IpczHandle CreateNonBrokerNode() {
    return MultinodeTest::CreateNonBrokerNode(GetParam());
  }

  void CreateTransports(IpczDriverHandle* transport0,
                        IpczDriverHandle* transport1) {
    MultinodeTest::CreateTransports(GetParam(), transport0, transport1);
  }

  IpczHandle ConnectToBroker(IpczHandle non_broker_node,
                             IpczDriverHandle transport) {
    return MultinodeTest::ConnectToBroker(GetParam(), non_broker_node,
                                          transport);
  }

  void Connect(IpczHandle broker_node,
               IpczHandle non_broker_node,
               IpczHandle* broker_portal,
               IpczHandle* non_broker_portal) {
    return MultinodeTest::Connect(broker_node, non_broker_node, GetParam(),
                                  broker_portal, non_broker_portal);
  }
};

}  // namespace test
}  // namespace ipcz

#define INSTANTIATE_MULTINODE_TEST_SUITE_P(suite)        \
  INSTANTIATE_TEST_SUITE_P(                              \
      , suite,                                           \
      ::testing::Values(                                 \
          ipcz::test::MultinodeTest::DriverMode::kSync,  \
          ipcz::test::MultinodeTest::DriverMode::kAsync, \
          ipcz::test::MultinodeTest::DriverMode::kAsyncDelegatedAlloc))

#endif  // IPCZ_SRC_TEST_MULTINODE_TEST_H_
