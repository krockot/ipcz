// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/api_test.h"

#include <cstdint>
#include <utility>

#include "core/portal.h"
#include "core/router.h"
#include "core/router_tracker.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "os/process.h"
#include "reference_drivers/event.h"
#include "testing/gtest/include/gtest/gtest.h"

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
  if (core::RouterTracker::GetNumRouters() != 0) {
    core::RouterTracker::DumpRouters();
  }
  ABSL_ASSERT(core::RouterTracker::GetNumRouters() == 0);
}

IpczHandle TestBase::ConnectNode(IpczHandle node,
                                 IpczDriverHandle driver_transport,
                                 const os::Process& process,
                                 IpczConnectNodeFlags flags) {
  IpczOSProcessHandle ipcz_process = {sizeof(ipcz_process)};
  const bool has_process =
      process.is_valid() &&
      os::Process::ToIpczOSProcessHandle(process.Clone(), ipcz_process);

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
  return ConnectNode(node, driver_transport, os::Process(),
                     IPCZ_CONNECT_NODE_TO_BROKER);
}

IpczHandle TestBase::ConnectToNonBroker(IpczHandle node,
                                        IpczDriverHandle driver_transport,
                                        const os::Process& process) {
  return ConnectNode(node, driver_transport, process, IPCZ_NO_FLAGS);
}

void TestBase::OpenPortals(IpczHandle node, IpczHandle* a, IpczHandle* b) {
  ASSERT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node, IPCZ_NO_FLAGS, nullptr, a, b));
}

void TestBase::Put(IpczHandle portal,
                   const std::string& str,
                   absl::Span<IpczHandle> portals,
                   absl::Span<os::Handle> os_handles) {
  std::vector<IpczOSHandle> handles(os_handles.size());
  for (size_t i = 0; i < os_handles.size(); ++i) {
    handles[i].size = sizeof(handles[i]);
    os::Handle::ToIpczOSHandle(std::move(os_handles[i]), &handles[i]);
  }

  ASSERT_EQ(IPCZ_RESULT_OK,
            ipcz.Put(portal, str.data(), static_cast<uint32_t>(str.length()),
                     portals.data(), static_cast<uint32_t>(portals.size()),
                     handles.data(), static_cast<uint32_t>(handles.size()),
                     IPCZ_NO_FLAGS, nullptr));
}

IpczResult TestBase::MaybeGet(IpczHandle portal, Parcel& parcel) {
  uint32_t num_bytes = 0;
  uint32_t num_portals = 0;
  uint32_t num_os_handles = 0;
  IpczResult result =
      ipcz.Get(portal, IPCZ_NO_FLAGS, nullptr, nullptr, &num_bytes, nullptr,
               &num_portals, nullptr, &num_os_handles);
  if (result == IPCZ_RESULT_OK) {
    parcel = {};
    return IPCZ_RESULT_OK;
  }

  if (result == IPCZ_RESULT_RESOURCE_EXHAUSTED) {
    std::vector<char> data(num_bytes);
    std::vector<IpczOSHandle> ipcz_os_handles(num_os_handles);
    parcel.portals.resize(num_portals);
    ABSL_ASSERT(result == IPCZ_RESULT_RESOURCE_EXHAUSTED);
    result = ipcz.Get(portal, IPCZ_NO_FLAGS, nullptr, data.data(), &num_bytes,
                      parcel.portals.data(), &num_portals,
                      ipcz_os_handles.data(), &num_os_handles);
    ABSL_ASSERT(result == IPCZ_RESULT_OK);

    parcel.message = {data.begin(), data.end()};
    parcel.os_handles.resize(num_os_handles);
    for (size_t i = 0; i < num_os_handles; ++i) {
      parcel.os_handles[i] = os::Handle::FromIpczOSHandle(ipcz_os_handles[i]);
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
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.DestroyTrap(trap, IPCZ_NO_FLAGS, nullptr));

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

  Put(a, kMessage1, {}, {});
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, p));
  EXPECT_EQ(kMessage1, p.message);

  Put(b, kMessage2, {}, {});
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(a, p));
  EXPECT_EQ(kMessage2, p.message);
}

bool TestBase::PortalsAreLocalPeers(IpczHandle a, IpczHandle b) {
  mem::Ref<core::Router> router_a =
      reinterpret_cast<core::Portal*>(a)->router();
  mem::Ref<core::Router> router_b =
      reinterpret_cast<core::Portal*>(b)->router();
  return router_a->HasLocalPeer(router_b);
}

void TestBase::LogPortalRoute(IpczHandle a) {
  mem::Ref<core::Router> router = reinterpret_cast<core::Portal*>(a)->router();
  router->LogRouteTrace();
}

// static
size_t TestBase::GetNumRouters() {
  return core::RouterTracker::GetNumRouters();
}

// static
void TestBase::DumpAllRouters() {
  return core::RouterTracker::DumpRouters();
}

}  // namespace test
}  // namespace ipcz
