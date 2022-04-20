// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/api_test.h"

#include <cstdint>
#include <utility>

#include "api.h"
#include "ipcz/ipcz.h"
#include "ipcz/node.h"
#include "ipcz/portal.h"
#include "ipcz/router.h"
#include "ipcz/router_tracker.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/synchronization/notification.h"
#include "util/ref_counted.h"

namespace ipcz::test {

TestBase::Parcel::Parcel() = default;

TestBase::Parcel::Parcel(Parcel&&) = default;

TestBase::Parcel& TestBase::Parcel::operator=(Parcel&&) = default;

TestBase::Parcel::~Parcel() = default;

TestBase::HangTimeout::HangTimeout(absl::Duration timeout,
                                   std::function<void()> handler)
    : thread_([this, timeout, handler = std::move(handler)] {
        if (!notification_.WaitForNotificationWithTimeout(timeout)) {
          handler();
        }
      }) {}

TestBase::HangTimeout::~HangTimeout() {
  notification_.Notify();
  thread_.join();
}

TestBase::TestBase() {
  ipcz.size = sizeof(ipcz);
  IpczGetAPI(&ipcz);
}

TestBase::~TestBase() {
  if (RouterTracker::GetNumRouters() != 0) {
    RouterTracker::DumpRouters();
  }
  ABSL_ASSERT(RouterTracker::GetNumRouters() == 0);
}

IpczHandle TestBase::ConnectNode(IpczHandle node,
                                 IpczDriverHandle driver_transport,
                                 IpczConnectNodeFlags flags) {
  const uint32_t num_initial_portals = 1;
  IpczHandle initial_portal;
  IpczResult result =
      ipcz.ConnectNode(node, driver_transport, num_initial_portals, flags,
                       nullptr, &initial_portal);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
  return initial_portal;
}

IpczHandle TestBase::ConnectToBroker(IpczHandle node,
                                     IpczDriverHandle driver_transport) {
  return ConnectNode(node, driver_transport, IPCZ_CONNECT_NODE_TO_BROKER);
}

IpczHandle TestBase::ConnectToNonBroker(IpczHandle node,
                                        IpczDriverHandle driver_transport) {
  return ConnectNode(node, driver_transport, IPCZ_NO_FLAGS);
}

void TestBase::OpenPortals(IpczHandle node, IpczHandle* a, IpczHandle* b) {
  ASSERT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node, IPCZ_NO_FLAGS, nullptr, a, b));
}

void TestBase::Put(IpczHandle portal,
                   const std::string& str,
                   absl::Span<IpczHandle> handles) {
  ASSERT_EQ(IPCZ_RESULT_OK,
            ipcz.Put(portal, str.data(), static_cast<uint32_t>(str.length()),
                     handles.data(), static_cast<uint32_t>(handles.size()),
                     IPCZ_NO_FLAGS, nullptr));
}

IpczResult TestBase::MaybeGet(IpczHandle portal, Parcel& parcel) {
  uint32_t num_bytes = 0;
  uint32_t num_handles = 0;
  IpczResult result = ipcz.Get(portal, IPCZ_NO_FLAGS, nullptr, nullptr,
                               &num_bytes, nullptr, &num_handles);
  if (result == IPCZ_RESULT_OK) {
    parcel = {};
    return IPCZ_RESULT_OK;
  }

  if (result == IPCZ_RESULT_RESOURCE_EXHAUSTED) {
    std::vector<char> data(num_bytes);
    parcel.handles.resize(num_handles);
    ABSL_ASSERT(result == IPCZ_RESULT_RESOURCE_EXHAUSTED);
    result = ipcz.Get(portal, IPCZ_NO_FLAGS, nullptr, data.data(), &num_bytes,
                      parcel.handles.data(), &num_handles);
    ABSL_ASSERT(result == IPCZ_RESULT_OK);

    parcel.message = {data.begin(), data.end()};
    return IPCZ_RESULT_OK;
  }

  return result;
}

IpczResult TestBase::WaitForIncomingParcel(IpczHandle portal) {
  absl::Notification notification;
  IpczTrapConditions conditions = {
      .size = sizeof(conditions),
      .flags = IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS | IPCZ_TRAP_DEAD,
      .min_local_parcels = 0,
  };
  IpczResult result = Trap(
      portal, conditions,
      [&notification](const IpczTrapEvent& event) { notification.Notify(); });
  if (result == IPCZ_RESULT_OK) {
    notification.WaitForNotification();
  }
  if (result == IPCZ_RESULT_FAILED_PRECONDITION) {
    return IPCZ_RESULT_OK;
  }
  return result;
}

IpczResult TestBase::WaitToGet(IpczHandle portal, Parcel& parcel) {
  IpczResult result = WaitForIncomingParcel(portal);
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  return MaybeGet(portal, parcel);
}

TestBase::Parcel TestBase::Get(IpczHandle portal) {
  Parcel parcel;
  IpczResult result = MaybeGet(portal, parcel);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
  return parcel;
}

bool TestBase::DiscardNextParcel(IpczHandle portal) {
  Parcel p;
  IpczResult result = MaybeGet(portal, p);
  if (result == IPCZ_RESULT_OK) {
    return true;
  }

  if (result != IPCZ_RESULT_UNAVAILABLE) {
    return false;
  }

  return WaitToGet(portal, p) == IPCZ_RESULT_OK;
}

IpczResult TestBase::Trap(IpczHandle portal,
                          const IpczTrapConditions& conditions,
                          Handler handler,
                          IpczTrapConditionFlags* satisfied_condition_flags,
                          IpczPortalStatus* status) {
  auto context = static_cast<uint64_t>(
      reinterpret_cast<uintptr_t>(new Handler(std::move(handler))));
  return ipcz.Trap(portal, &conditions, &OnTrapEvent, context, IPCZ_NO_FLAGS,
                   nullptr, satisfied_condition_flags, status);
}

void TestBase::VerifyEndToEnd(IpczHandle a, IpczHandle b) {
  Parcel p;
  const std::string kMessage1 = "psssst";
  const std::string kMessage2 = "ssshhh";

  Put(a, kMessage1);
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, p));
  EXPECT_EQ(kMessage1, p.message);

  Put(b, kMessage2);
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
  EXPECT_EQ(kMessage2, p.message);
}

bool TestBase::PortalsAreLocalPeers(IpczHandle a, IpczHandle b) {
  Ref<Router> router_a = reinterpret_cast<Portal*>(a)->router();
  Ref<Router> router_b = reinterpret_cast<Portal*>(b)->router();
  return router_a->HasLocalPeer(router_b);
}

void TestBase::LogPortalRoute(IpczHandle a) {
  Ref<Router> router = reinterpret_cast<Portal*>(a)->router();
  router->LogRouteTrace();
}

// static
size_t TestBase::GetNumRouters() {
  return RouterTracker::GetNumRouters();
}

// static
void TestBase::DumpAllRouters() {
  return RouterTracker::DumpRouters();
}

// static
void TestBase::DiagnoseNode(IpczHandle node) {
  reinterpret_cast<ipcz::Node*>(static_cast<uintptr_t>(node))
      ->DiagnoseForTesting();
}

// static
void TestBase::OnTrapEvent(const IpczTrapEvent* event) {
  std::unique_ptr<Handler> handler(
      reinterpret_cast<Handler*>(static_cast<uintptr_t>(event->context)));
  (*handler)(*event);
}

}  // namespace ipcz::test
