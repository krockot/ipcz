// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <functional>

#include "drivers/single_process_reference_driver.h"
#include "ipcz/ipcz.h"
#include "test/test_base.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

class TrapTest : public test::TestBase {
 public:
  TrapTest() {
    ipcz.CreateNode(&drivers::kSingleProcessReferenceDriver,
                    IPCZ_INVALID_DRIVER_HANDLE, IPCZ_CREATE_NODE_AS_BROKER,
                    nullptr, &node);
  }

  ~TrapTest() override { ipcz.DestroyNode(node, IPCZ_NO_FLAGS, nullptr); }

  IpczHandle node;
};

class TestTrap {
 public:
  TestTrap(const IpczAPI& ipcz,
           IpczHandle portal,
           const IpczTrapConditions& conditions,
           std::function<void(const IpczTrapEvent&)> handler)
      : ipcz_(ipcz), handler_(std::move(handler)) {
    EXPECT_EQ(IPCZ_RESULT_OK,
              ipcz_.CreateTrap(portal, &conditions, &TestTrap::OnEvent,
                               context(), IPCZ_NO_FLAGS, nullptr, &trap_));
  }

  ~TestTrap() {
    if (!destroyed_) {
      Destroy();
    }
  }

  uintptr_t context() const {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this));
  }

  static void OnEvent(const IpczTrapEvent* event) {
    reinterpret_cast<TestTrap*>(static_cast<uintptr_t>(event->context))
        ->handler_(*event);
  }

  IpczResult Arm(IpczTrapConditionFlags* satisfied_flags = nullptr,
                 IpczPortalStatus* status = nullptr) {
    return ipcz_.ArmTrap(trap_, IPCZ_NO_FLAGS, nullptr, satisfied_flags,
                         status);
  }

  void Destroy() {
    destroyed_ = true;
    EXPECT_EQ(IPCZ_RESULT_OK, ipcz_.DestroyTrap(trap_, IPCZ_NO_FLAGS, nullptr));
  }

  void DestroyBlocking() {
    destroyed_ = true;
    EXPECT_EQ(IPCZ_RESULT_OK,
              ipcz_.DestroyTrap(trap_, IPCZ_DESTROY_TRAP_BLOCKING, nullptr));
  }

 private:
  const IpczAPI& ipcz_;
  const std::function<void(const IpczTrapEvent&)> handler_;
  IpczHandle trap_;
  bool destroyed_ = false;
};

TEST_F(TrapTest, BasicTrigger) {
  IpczHandle a, b;
  OpenPortals(node, &a, &b);

  bool tripped = false;
  IpczTrapConditions conditions = {sizeof(conditions)};
  conditions.flags = IPCZ_TRAP_CONDITION_LOCAL_PARCELS;
  conditions.min_local_parcels = 1;
  TestTrap trap(ipcz, b, conditions, [&trap, &tripped](const IpczTrapEvent& e) {
    EXPECT_EQ(trap.context(), e.context);
    EXPECT_TRUE((e.condition_flags & IPCZ_TRAP_CONDITION_LOCAL_PARCELS) != 0);
    tripped = true;
  });

  EXPECT_EQ(IPCZ_RESULT_OK, trap.Arm());
  Put(a, "hello", {}, {});
  EXPECT_TRUE(tripped);
  ClosePortals({a, b});
}

TEST_F(TrapTest, NestedTrigger) {
  IpczHandle a, b;
  OpenPortals(node, &a, &b);

  bool tripped_a = false;
  bool tripped_b = false;
  IpczTrapConditions conditions = {sizeof(conditions)};
  conditions.flags = IPCZ_TRAP_CONDITION_LOCAL_PARCELS;
  conditions.min_local_parcels = 1;
  TestTrap trap_a(ipcz, a, conditions,
                  [&trap_a, &tripped_a, &tripped_b](const IpczTrapEvent& e) {
                    EXPECT_EQ(trap_a.context(), e.context);
                    EXPECT_TRUE(tripped_b);
                    tripped_a = true;
                  });
  TestTrap trap_b(
      ipcz, b, conditions,
      [this, &b, &trap_b, &tripped_a, &tripped_b](const IpczTrapEvent& e) {
        EXPECT_EQ(trap_b.context(), e.context);
        tripped_b = true;
        EXPECT_FALSE(tripped_a);
        Put(b, "pong");
        EXPECT_TRUE(tripped_a);
      });

  EXPECT_EQ(IPCZ_RESULT_OK, trap_a.Arm());
  EXPECT_EQ(IPCZ_RESULT_OK, trap_b.Arm());
  Put(a, "ping");
  EXPECT_TRUE(tripped_b);
  EXPECT_TRUE(tripped_a);

  ClosePortals({a, b});
}

TEST_F(TrapTest, DestroyInTrigger) {
  IpczHandle a, b;
  OpenPortals(node, &a, &b);

  bool tripped = false;
  IpczTrapConditions conditions = {sizeof(conditions)};
  conditions.flags = IPCZ_TRAP_CONDITION_LOCAL_PARCELS;
  conditions.min_local_parcels = 1;
  TestTrap trap(ipcz, b, conditions, [&trap, &tripped](const IpczTrapEvent& e) {
    EXPECT_FALSE(tripped);
    EXPECT_EQ(trap.context(), e.context);
    EXPECT_TRUE((e.condition_flags & IPCZ_TRAP_CONDITION_LOCAL_PARCELS) != 0);
    tripped = true;
    trap.Destroy();
  });

  EXPECT_EQ(IPCZ_RESULT_OK, trap.Arm());
  Put(a, "hello");
  EXPECT_TRUE(tripped);
  ClosePortals({a, b});
}

TEST_F(TrapTest, RearmInEventHandler) {
  IpczHandle a, b;
  OpenPortals(node, &a, &b);

  bool tripped = false;
  IpczTrapConditions conditions = {sizeof(conditions)};
  conditions.flags = IPCZ_TRAP_CONDITION_LOCAL_PARCELS;
  conditions.min_local_parcels = 1;
  TestTrap trap(ipcz, b, conditions, [&trap, &tripped](const IpczTrapEvent& e) {
    EXPECT_FALSE(tripped);
    EXPECT_EQ(trap.context(), e.context);
    EXPECT_TRUE((e.condition_flags & IPCZ_TRAP_CONDITION_LOCAL_PARCELS) != 0);
    tripped = true;
    trap.Destroy();
  });

  EXPECT_EQ(IPCZ_RESULT_OK, trap.Arm());
  Put(a, "hello");
  EXPECT_TRUE(tripped);
  ClosePortals({a, b});
}

}  // namespace
}  // namespace ipcz
