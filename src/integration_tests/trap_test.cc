// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <chrono>
#include <thread>

#include "ipcz/ipcz.h"
#include "reference_drivers/event.h"
#include "reference_drivers/single_process_reference_driver.h"
#include "test/test_base.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "util/function.h"

namespace ipcz {
namespace {

class TrapTest : public test::TestBase {
 public:
  TrapTest() {
    ipcz.CreateNode(&reference_drivers::kSingleProcessReferenceDriver,
                    IPCZ_INVALID_DRIVER_HANDLE, IPCZ_CREATE_NODE_AS_BROKER,
                    nullptr, &node);
  }

  ~TrapTest() override { ipcz.Close(node, IPCZ_NO_FLAGS, nullptr); }

  IpczHandle node;
};

TEST_F(TrapTest, BasicTrigger) {
  IpczHandle a, b;
  OpenPortals(node, &a, &b);

  bool tripped = false;
  IpczTrapConditions conditions = {
      .size = sizeof(conditions),
      .flags = IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS,
      .min_local_parcels = 0,
  };

  EXPECT_EQ(
      IPCZ_RESULT_OK, Trap(b, conditions, [&tripped](const IpczTrapEvent& e) {
        EXPECT_NE(0u, e.condition_flags & IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS);
        tripped = true;
      }));

  Put(a, "hello");
  EXPECT_TRUE(tripped);
  ClosePortals({a, b});
}

TEST_F(TrapTest, MinLocalParcels) {
  IpczHandle a, b;
  OpenPortals(node, &a, &b);

  bool tripped = false;
  IpczTrapConditions conditions = {
      .size = sizeof(conditions),
      .flags = IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS,
      .min_local_parcels = 2,
  };
  EXPECT_EQ(
      IPCZ_RESULT_OK, Trap(b, conditions, [&tripped](const IpczTrapEvent& e) {
        EXPECT_NE(0u, e.condition_flags & IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS);
        tripped = true;
      }));

  Put(a, "hello");
  EXPECT_FALSE(tripped);
  Put(a, "ummmm...hello?");
  EXPECT_FALSE(tripped);
  Put(a, "HEY.");
  EXPECT_TRUE(tripped);
  ClosePortals({a, b});
}

TEST_F(TrapTest, MinLocalBytes) {
  IpczHandle a, b;
  OpenPortals(node, &a, &b);

  bool tripped = false;
  IpczTrapConditions conditions = {
      .size = sizeof(conditions),
      .flags = IPCZ_TRAP_ABOVE_MIN_LOCAL_BYTES,
      .min_local_bytes = 10,
  };
  EXPECT_EQ(
      IPCZ_RESULT_OK, Trap(b, conditions, [&tripped](const IpczTrapEvent& e) {
        EXPECT_NE(0u, e.condition_flags & IPCZ_TRAP_ABOVE_MIN_LOCAL_BYTES);
        tripped = true;
      }));

  Put(a, "abcde");
  EXPECT_FALSE(tripped);
  Put(a, "fghij");
  EXPECT_FALSE(tripped);
  Put(a, "k");
  EXPECT_TRUE(tripped);
  ClosePortals({a, b});
}

TEST_F(TrapTest, NestedTrigger) {
  IpczHandle a, b;
  OpenPortals(node, &a, &b);

  bool tripped_a = false;
  bool tripped_b = false;
  IpczTrapConditions conditions = {
      .size = sizeof(conditions),
      .flags = IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS,
      .min_local_parcels = 0,
  };

  EXPECT_EQ(
      IPCZ_RESULT_OK,
      Trap(a, conditions, [&tripped_a, &tripped_b](const IpczTrapEvent& e) {
        EXPECT_TRUE(tripped_b);
        tripped_a = true;
      }));

  EXPECT_EQ(IPCZ_RESULT_OK,
            Trap(b, conditions,
                 [this, &b, &tripped_a, &tripped_b](const IpczTrapEvent& e) {
                   tripped_b = true;
                   EXPECT_FALSE(tripped_a);
                   Put(b, "pong");
                   EXPECT_TRUE(tripped_a);
                 }));

  Put(a, "ping");
  EXPECT_TRUE(tripped_b);
  EXPECT_TRUE(tripped_a);

  ClosePortals({a, b});
}

TEST_F(TrapTest, TrapConditionsAlreadyMet) {
  IpczHandle a, b;
  OpenPortals(node, &a, &b);

  IpczTrapConditions conditions = {
      .size = sizeof(conditions),
      .flags = IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS,
      .min_local_parcels = 0,
  };

  Put(a, "hello");

  IpczTrapConditionFlags flags;
  IpczPortalStatus status = {sizeof(status)};
  EXPECT_EQ(IPCZ_RESULT_FAILED_PRECONDITION,
            Trap(
                b, conditions, [](const IpczTrapEvent& e) {}, &flags, &status));
  EXPECT_TRUE((flags & IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS) != 0);
  EXPECT_EQ(1u, status.num_local_parcels);

  ClosePortals({a, b});
}

TEST_F(TrapTest, AutomaticRemoval) {
  IpczHandle a, b;
  OpenPortals(node, &a, &b);

  bool tripped = false;
  IpczTrapConditions conditions = {
      .size = sizeof(conditions),
      .flags = IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS,
      .min_local_parcels = 0,
  };
  EXPECT_EQ(IPCZ_RESULT_OK,
            Trap(a, conditions, [&tripped](const IpczTrapEvent& e) {
              EXPECT_NE(0u, e.condition_flags & IPCZ_TRAP_REMOVED);
              tripped = true;
            }));

  EXPECT_FALSE(tripped);
  ClosePortals({a});
  EXPECT_TRUE(tripped);
  ClosePortals({b});
}

}  // namespace
}  // namespace ipcz
