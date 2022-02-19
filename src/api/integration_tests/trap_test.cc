// Copyright 2021 The Chromium Authors. All rights reserved.
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

  ~TrapTest() override { ipcz.DestroyNode(node, IPCZ_NO_FLAGS, nullptr); }

  IpczHandle node;
};

class TestTrap {
 public:
  TestTrap(const IpczAPI& ipcz,
           IpczHandle portal,
           const IpczTrapConditions& conditions,
           Function<void(const IpczTrapEvent&)> handler)
      : ipcz_(ipcz), handler_(std::move(handler)) {
    EXPECT_EQ(IPCZ_RESULT_OK,
              ipcz_.CreateTrap(portal, &conditions, &TestTrap::OnEvent,
                               context(), IPCZ_NO_FLAGS, nullptr, &trap_));
  }

  ~TestTrap() {
    if (!destroyed_) {
      EXPECT_EQ(IPCZ_RESULT_OK, Destroy());
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

  IpczResult Destroy() {
    destroyed_ = true;
    return ipcz_.DestroyTrap(trap_, IPCZ_NO_FLAGS, nullptr);
  }

  IpczResult DestroyBlocking() {
    destroyed_ = true;
    return ipcz_.DestroyTrap(trap_, IPCZ_DESTROY_TRAP_BLOCKING, nullptr);
  }

 private:
  const IpczAPI& ipcz_;
  const Function<void(const IpczTrapEvent&)> handler_;
  IpczHandle trap_;
  bool destroyed_ = false;
};

TEST_F(TrapTest, BasicTrigger) {
  IpczHandle a, b;
  OpenPortals(node, &a, &b);

  bool tripped = false;
  IpczTrapConditions conditions = {sizeof(conditions)};
  conditions.flags = IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS;
  conditions.min_local_parcels = 0;
  TestTrap trap(ipcz, b, conditions, [&trap, &tripped](const IpczTrapEvent& e) {
    EXPECT_EQ(trap.context(), e.context);
    EXPECT_TRUE((e.condition_flags & IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS) != 0);
    tripped = true;
  });

  EXPECT_EQ(IPCZ_RESULT_OK, trap.Arm());
  Put(a, "hello", {}, {});
  EXPECT_TRUE(tripped);
  ClosePortals({a, b});
}

TEST_F(TrapTest, MinLocalParcels) {
  IpczHandle a, b;
  OpenPortals(node, &a, &b);

  bool tripped = false;
  IpczTrapConditions conditions = {sizeof(conditions)};
  conditions.flags = IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS;
  conditions.min_local_parcels = 2;
  TestTrap trap(ipcz, b, conditions, [&trap, &tripped](const IpczTrapEvent& e) {
    EXPECT_EQ(trap.context(), e.context);
    EXPECT_TRUE((e.condition_flags & IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS) != 0);
    tripped = true;
  });

  EXPECT_EQ(IPCZ_RESULT_OK, trap.Arm());
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
  IpczTrapConditions conditions = {sizeof(conditions)};
  conditions.flags = IPCZ_TRAP_ABOVE_MIN_LOCAL_BYTES;
  conditions.min_local_bytes = 10;
  TestTrap trap(ipcz, b, conditions, [&trap, &tripped](const IpczTrapEvent& e) {
    EXPECT_EQ(trap.context(), e.context);
    EXPECT_TRUE((e.condition_flags & IPCZ_TRAP_ABOVE_MIN_LOCAL_BYTES) != 0);
    tripped = true;
  });

  EXPECT_EQ(IPCZ_RESULT_OK, trap.Arm());
  Put(a, "abcde");
  EXPECT_FALSE(tripped);
  Put(a, "fghij");
  EXPECT_FALSE(tripped);
  Put(a, "k");
  EXPECT_TRUE(tripped);
  ClosePortals({a, b});
}

TEST_F(TrapTest, NewLocalParcel) {
  IpczHandle a, b;
  OpenPortals(node, &a, &b);

  Put(a, "it's very sunny");
  Put(a, "there's lots of pineapples there");

  bool tripped = false;
  IpczTrapConditions conditions = {sizeof(conditions)};
  conditions.flags = IPCZ_TRAP_NEW_LOCAL_PARCEL;
  TestTrap trap(ipcz, b, conditions, [&trap, &tripped](const IpczTrapEvent& e) {
    EXPECT_EQ(trap.context(), e.context);
    EXPECT_TRUE((e.condition_flags & IPCZ_TRAP_NEW_LOCAL_PARCEL) != 0);
    tripped = true;
  });

  // There are already parcels queued locally, but arming must succeed because
  // the only trapped condition is the arrival of a new parcel.
  EXPECT_EQ(IPCZ_RESULT_OK, trap.Arm());

  Put(a, "boop");
  EXPECT_TRUE(tripped);

  // It's not armed anymore, so it shouldn't trip again.
  tripped = false;
  Put(a, "beep");
  EXPECT_FALSE(tripped);

  // But it should be immediately re-armable and should trip with a new parcel
  // again.
  EXPECT_EQ(IPCZ_RESULT_OK, trap.Arm());
  Put(a, "bzzzzzt");
  EXPECT_TRUE(tripped);

  ClosePortals({a, b});
}

TEST_F(TrapTest, NestedTrigger) {
  IpczHandle a, b;
  OpenPortals(node, &a, &b);

  bool tripped_a = false;
  bool tripped_b = false;
  IpczTrapConditions conditions = {sizeof(conditions)};
  conditions.flags = IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS;
  conditions.min_local_parcels = 0;
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
  conditions.flags = IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS;
  conditions.min_local_parcels = 0;
  TestTrap trap(ipcz, b, conditions, [&trap, &tripped](const IpczTrapEvent& e) {
    EXPECT_FALSE(tripped);
    EXPECT_EQ(trap.context(), e.context);
    EXPECT_TRUE((e.condition_flags & IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS) != 0);
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
  conditions.flags = IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS;
  conditions.min_local_parcels = 0;
  TestTrap trap(
      ipcz, b, conditions, [this, b, &trap, &tripped](const IpczTrapEvent& e) {
        EXPECT_FALSE(tripped);
        EXPECT_EQ(trap.context(), e.context);
        EXPECT_TRUE((e.condition_flags & IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS) !=
                    0);
        tripped = true;
        EXPECT_EQ(IPCZ_RESULT_FAILED_PRECONDITION, trap.Arm());

        Parcel p;
        EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, p));
        EXPECT_EQ("hello", p.message);

        EXPECT_EQ(IPCZ_RESULT_OK, trap.Arm());
      });

  EXPECT_EQ(IPCZ_RESULT_OK, trap.Arm());
  Put(a, "hello");
  EXPECT_TRUE(tripped);

  tripped = false;
  Put(a, "hello");
  EXPECT_TRUE(tripped);

  ClosePortals({a, b});
}

TEST_F(TrapTest, ArmWithConditionsMet) {
  IpczHandle a, b;
  OpenPortals(node, &a, &b);

  IpczTrapConditions conditions = {sizeof(conditions)};
  conditions.flags = IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS;
  conditions.min_local_parcels = 0;
  TestTrap trap(ipcz, b, conditions, [](const IpczTrapEvent& e) {});

  Put(a, "hello");

  IpczTrapConditionFlags flags;
  IpczPortalStatus status = {sizeof(status)};
  EXPECT_EQ(IPCZ_RESULT_FAILED_PRECONDITION, trap.Arm(&flags, &status));
  EXPECT_TRUE((flags & IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS) != 0);
  EXPECT_EQ(1u, status.num_local_parcels);

  ClosePortals({a, b});
}

TEST_F(TrapTest, NoDispatchAfterDestroy) {
  IpczHandle a, b;
  OpenPortals(node, &a, &b);

  bool tripped = false;
  IpczTrapConditions conditions = {sizeof(conditions)};
  conditions.flags = IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS;
  conditions.min_local_parcels = 0;
  TestTrap trap(ipcz, b, conditions,
                [&tripped](const IpczTrapEvent& e) { tripped = true; });

  EXPECT_EQ(IPCZ_RESULT_OK, trap.Arm());
  EXPECT_EQ(IPCZ_RESULT_OK, trap.Destroy());
  Put(a, "hello", {}, {});
  EXPECT_FALSE(tripped);

  ClosePortals({a, b});
}

TEST_F(TrapTest, DestroyBlocking) {
  IpczHandle a, b;
  OpenPortals(node, &a, &b);

  reference_drivers::Event trap_event_fired;
  reference_drivers::Event::Notifier trap_event_notifier =
      trap_event_fired.MakeNotifier();
  bool tripped = false;
  IpczTrapConditions conditions = {sizeof(conditions)};
  conditions.flags = IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS;
  conditions.min_local_parcels = 0;
  TestTrap trap(ipcz, b, conditions,
                [&trap_event_notifier, &tripped](const IpczTrapEvent& e) {
                  using namespace std::chrono_literals;
                  trap_event_notifier.Notify();
                  std::this_thread::sleep_for(10ms);
                  tripped = true;
                });

  EXPECT_EQ(IPCZ_RESULT_OK, trap.Arm());

  // Trigger the trap on a background thread.
  std::thread t([this, a] { Put(a, "hello"); });

  // Wait for the trap handler to be invoked and then immediately destroy the
  // trap. DestroyBlocking() must wait for the handler to complete before
  // returning.
  trap_event_fired.Wait();
  EXPECT_EQ(IPCZ_RESULT_OK, trap.DestroyBlocking());
  EXPECT_TRUE(tripped);
  t.join();

  ClosePortals({a, b});
}

}  // namespace
}  // namespace ipcz
