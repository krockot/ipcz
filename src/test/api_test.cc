// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/api_test.h"

#include "drivers/multiprocess_reference_driver.h"
#include "drivers/single_process_reference_driver.h"
#include "test/test_client.h"

namespace ipcz {
namespace test {

APITest::Parcel::Parcel() = default;

APITest::Parcel::Parcel(Parcel&&) = default;

APITest::Parcel& APITest::Parcel::operator=(Parcel&&) = default;

APITest::Parcel::~Parcel() = default;

APITest::APITest() {
  ipcz.size = sizeof(ipcz);
  IpczGetAPI(&ipcz);

  IpczCreateNodeFlags flags = 0;
  if (!TestClient::InClientProcess()) {
    flags |= IPCZ_CREATE_NODE_AS_BROKER;
  }
  ipcz.CreateNode(&drivers::kSingleProcessReferenceDriver,
                  IPCZ_INVALID_DRIVER_HANDLE, flags, nullptr, &node_);
  OpenPortals(&q, &p);
}

APITest::~APITest() {
  ClosePortals({q, p});
  ipcz.DestroyNode(node_, IPCZ_NO_FLAGS, nullptr);
}

IpczHandle APITest::CreateSingleProcessNode(IpczCreateNodeFlags flags) {
  IpczHandle node;
  ipcz.CreateNode(&drivers::kSingleProcessReferenceDriver,
                  IPCZ_INVALID_DRIVER_HANDLE, flags, nullptr, &node);
  return node;
}

void APITest::CreateSingleProcessTransports(IpczDriverHandle* first,
                                            IpczDriverHandle* second) {
  drivers::kSingleProcessReferenceDriver.CreateTransports(
      IPCZ_INVALID_DRIVER_HANDLE, IPCZ_NO_FLAGS, nullptr, first, second);
}

IpczHandle APITest::ConnectNode(IpczHandle node,
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

IpczHandle APITest::ConnectToBroker(IpczHandle node,
                                    IpczDriverHandle driver_transport) {
  return ConnectNode(node, driver_transport, os::Process(),
                     IPCZ_CONNECT_NODE_TO_BROKER);
}

IpczHandle APITest::ConnectToNonBroker(IpczHandle node,
                                       IpczDriverHandle driver_transport,
                                       const os::Process& process) {
  return ConnectNode(node, driver_transport, process, IPCZ_NO_FLAGS);
}

void APITest::ConnectSingleProcessBrokerToNonBroker(
    IpczHandle broker,
    IpczHandle non_broker,
    IpczHandle* broker_portal,
    IpczHandle* non_broker_portal) {
  IpczDriverHandle transport0;
  IpczDriverHandle transport1;
  CreateSingleProcessTransports(&transport0, &transport1);
  *broker_portal =
      ConnectToNonBroker(broker, transport0, os::Process::GetCurrent());
  *non_broker_portal = ConnectToBroker(non_broker, transport1);
}

IpczHandle APITest::CreateMultiprocessNode(IpczCreateNodeFlags flags) {
  IpczHandle node;
  ipcz.CreateNode(&drivers::kMultiprocessReferenceDriver,
                  IPCZ_INVALID_DRIVER_HANDLE, flags, nullptr, &node);
  return node;
}

IpczDriverHandle APITest::CreateMultiprocessTransport(os::Channel& channel) {
  IpczOSHandle os_handle;
  if (!os::Handle::ToIpczOSHandle(channel.TakeHandle(), &os_handle)) {
    return IPCZ_INVALID_DRIVER_HANDLE;
  }

  IpczDriverHandle transport;
  IpczResult result =
      drivers::kMultiprocessReferenceDriver.DeserializeTransport(
          IPCZ_INVALID_DRIVER_HANDLE, nullptr, 0, &os_handle, 1, nullptr,
          IPCZ_NO_FLAGS, nullptr, &transport);
  if (result != IPCZ_RESULT_OK) {
    return IPCZ_INVALID_DRIVER_HANDLE;
  }

  return transport;
}

IpczHandle APITest::ConnectNode(IpczHandle node, TestClient& client) {
  return ConnectNode(node, client.channel(), client.process());
}

IpczHandle APITest::ConnectNode(IpczHandle node,
                                os::Channel& channel,
                                const os::Process& process) {
  IpczOSProcessHandle ipcz_process = {sizeof(ipcz_process)};
  os::Process::ToIpczOSProcessHandle(process.Clone(), ipcz_process);

  // TODO

  return IPCZ_INVALID_HANDLE;
}

void APITest::OpenPortals(IpczHandle* a, IpczHandle* b) {
  ASSERT_EQ(IPCZ_RESULT_OK,
            ipcz.OpenPortals(node_, IPCZ_NO_FLAGS, nullptr, a, b));
}

void APITest::Put(IpczHandle portal,
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

IpczResult APITest::MaybeGet(IpczHandle portal, Parcel& parcel) {
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
    ABSL_ASSERT(ipcz.Get(portal, IPCZ_NO_FLAGS, nullptr, data.data(),
                         &num_bytes, parcel.portals.data(), &num_portals,
                         ipcz_os_handles.data(),
                         &num_os_handles) == IPCZ_RESULT_OK);
    parcel.message = {data.begin(), data.end()};
    parcel.os_handles.resize(num_os_handles);
    for (size_t i = 0; i < num_os_handles; ++i) {
      parcel.os_handles[i] = os::Handle::FromIpczOSHandle(ipcz_os_handles[i]);
    }
    return IPCZ_RESULT_OK;
  }

  return result;
}

IpczResult APITest::WaitToGet(IpczHandle portal, Parcel& parcel) {
  os::Event event;
  os::Event::Notifier notifier = event.MakeNotifier();
  IpczTrapConditions conditions = {sizeof(conditions)};
  conditions.flags =
      IPCZ_TRAP_CONDITION_LOCAL_PARCELS | IPCZ_TRAP_CONDITION_DEAD;
  conditions.min_local_parcels = 1;
  const auto handler = [](const IpczTrapEvent* event) {
    reinterpret_cast<os::Event::Notifier*>(event->context)->Notify();
  };
  const auto context = reinterpret_cast<uintptr_t>(&notifier);
  IpczHandle trap;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.CreateTrap(portal, &conditions, handler, context,
                            IPCZ_NO_FLAGS, nullptr, &trap));
  IpczResult result =
      ipcz.ArmTrap(portal, trap, IPCZ_NO_FLAGS, nullptr, nullptr, nullptr);
  if (result == IPCZ_RESULT_OK) {
    event.Wait();
  } else if (result != IPCZ_RESULT_FAILED_PRECONDITION) {
    return result;
  }
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.DestroyTrap(portal, trap, IPCZ_NO_FLAGS, nullptr));

  return MaybeGet(portal, parcel);
}

APITest::Parcel APITest::Get(IpczHandle portal) {
  Parcel parcel;
  IpczResult result = MaybeGet(portal, parcel);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
  return parcel;
}

}  // namespace test
}  // namespace ipcz
