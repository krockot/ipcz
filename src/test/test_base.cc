// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/api_test.h"

#include <cstdint>
#include <utility>

#include "ipcz/ipcz.h"
#include "ipcz/portal.h"
#include "ipcz/router.h"
#include "ipcz/router_tracker.h"
#include "reference_drivers/event.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "util/os_handle.h"
#include "util/os_process.h"
#include "util/ref_counted.h"

namespace ipcz {
namespace test {

TestBase::Parcel::Parcel() = default;

TestBase::Parcel::Parcel(Parcel&&) = default;

TestBase::Parcel& TestBase::Parcel::operator=(Parcel&&) = default;

TestBase::Parcel::~Parcel() = default;

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
                                 const OSProcess& process,
                                 IpczConnectNodeFlags flags) {
  IpczOSProcessHandle ipcz_process = {sizeof(ipcz_process)};
  const bool has_process =
      process.is_valid() &&
      OSProcess::ToIpczOSProcessHandle(process.Clone(), ipcz_process);

  const uint32_t num_initial_portals = 1;
  IpczHandle initial_portal;
  IpczResult result = ipcz.ConnectNode(
      node, driver_transport, has_process ? &ipcz_process : nullptr,
      num_initial_portals, flags, nullptr, &initial_portal);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
  return initial_portal;
}

IpczHandle TestBase::ConnectToBroker(IpczHandle node,
                                     IpczDriverHandle driver_transport) {
  return ConnectNode(node, driver_transport, OSProcess(),
                     IPCZ_CONNECT_NODE_TO_BROKER);
}

IpczHandle TestBase::ConnectToNonBroker(IpczHandle node,
                                        IpczDriverHandle driver_transport,
                                        const OSProcess& process) {
  return ConnectNode(node, driver_transport, process, IPCZ_NO_FLAGS);
}

void TestBase::OpenPortals(IpczHandle node, IpczHandle* a, IpczHandle* b) {
  ASSERT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node, IPCZ_NO_FLAGS, nullptr, a, b));
}

void TestBase::Put(IpczHandle portal,
                   const std::string& str,
                   absl::Span<IpczHandle> handles,
                   absl::Span<OSHandle> os_handles) {
  std::vector<IpczOSHandle> ipcz_os_handles(os_handles.size());
  for (size_t i = 0; i < os_handles.size(); ++i) {
    ipcz_os_handles[i].size = sizeof(ipcz_os_handles[i]);
    OSHandle::ToIpczOSHandle(std::move(os_handles[i]), &ipcz_os_handles[i]);
  }

  ASSERT_EQ(IPCZ_RESULT_OK,
            ipcz.Put(portal, str.data(), static_cast<uint32_t>(str.length()),
                     handles.data(), static_cast<uint32_t>(handles.size()),
                     ipcz_os_handles.data(),
                     static_cast<uint32_t>(ipcz_os_handles.size()),
                     IPCZ_NO_FLAGS, nullptr));
}

IpczResult TestBase::MaybeGet(IpczHandle portal, Parcel& parcel) {
  uint32_t num_bytes = 0;
  uint32_t num_handles = 0;
  uint32_t num_os_handles = 0;
  IpczResult result =
      ipcz.Get(portal, IPCZ_NO_FLAGS, nullptr, nullptr, &num_bytes, nullptr,
               &num_handles, nullptr, &num_os_handles);
  if (result == IPCZ_RESULT_OK) {
    parcel = {};
    return IPCZ_RESULT_OK;
  }

  if (result == IPCZ_RESULT_RESOURCE_EXHAUSTED) {
    std::vector<char> data(num_bytes);
    std::vector<IpczOSHandle> ipcz_os_handles(num_os_handles);
    parcel.handles.resize(num_handles);
    ABSL_ASSERT(result == IPCZ_RESULT_RESOURCE_EXHAUSTED);
    result = ipcz.Get(portal, IPCZ_NO_FLAGS, nullptr, data.data(), &num_bytes,
                      parcel.handles.data(), &num_handles,
                      ipcz_os_handles.data(), &num_os_handles);
    ABSL_ASSERT(result == IPCZ_RESULT_OK);

    parcel.message = {data.begin(), data.end()};
    parcel.os_handles.resize(num_os_handles);
    for (size_t i = 0; i < num_os_handles; ++i) {
      parcel.os_handles[i] = OSHandle::FromIpczOSHandle(ipcz_os_handles[i]);
    }

    return IPCZ_RESULT_OK;
  }

  return result;
}

IpczResult TestBase::WaitToGet(IpczHandle portal, Parcel& parcel) {
  reference_drivers::Event event;
  reference_drivers::Event::Notifier notifier = event.MakeNotifier();
  IpczTrapConditions conditions = {sizeof(conditions)};
  conditions.flags = IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS | IPCZ_TRAP_DEAD;
  conditions.min_local_parcels = 0;
  const auto handler = [](const IpczTrapEvent* event) {
    reinterpret_cast<reference_drivers::Event::Notifier*>(event->context)
        ->Notify();
  };
  const auto context =
      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&notifier));
  IpczHandle trap;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.CreateTrap(portal, &conditions, handler, context,
                            IPCZ_NO_FLAGS, nullptr, &trap));
  IpczResult result =
      ipcz.ArmTrap(trap, IPCZ_NO_FLAGS, nullptr, nullptr, nullptr);
  if (result == IPCZ_RESULT_OK) {
    event.Wait();
  } else if (result != IPCZ_RESULT_FAILED_PRECONDITION) {
    return result;
  }
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Close(trap, IPCZ_NO_FLAGS, nullptr));

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

}  // namespace test
}  // namespace ipcz
