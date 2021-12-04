// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/portal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <tuple>
#include <utility>

#include "core/local_router_link.h"
#include "core/node.h"
#include "core/parcel.h"
#include "core/portal_descriptor.h"
#include "core/router.h"
#include "core/side.h"
#include "core/trap.h"
#include "core/two_sided.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/handle_util.h"

namespace ipcz {
namespace core {

namespace {

bool ValidateAndAcquirePortalsForTransitFrom(
    Portal& sender,
    absl::Span<const IpczHandle> handles,
    Parcel::PortalVector& portals) {
  portals.resize(handles.size());
  for (size_t i = 0; i < handles.size(); ++i) {
    auto portal = mem::WrapRefCounted(ToPtr<Portal>(handles[i]));
    if (&sender == portal.get() ||
        sender.router()->HasLocalPeer(portal->router())) {
      return false;
    }
    portals[i] = std::move(portal);
  }
  return true;
}

}  // namespace

Portal::Portal(mem::Ref<Node> node, mem::Ref<Router> router)
    : node_(std::move(node)), router_(std::move(router)) {}

Portal::~Portal() = default;

// static
TwoSided<mem::Ref<Portal>> Portal::CreatePair(mem::Ref<Node> node) {
  TwoSided<mem::Ref<Router>> routers{mem::MakeRefCounted<Router>(Side::kLeft),
                                     mem::MakeRefCounted<Router>(Side::kRight)};
  TwoSided<mem::Ref<RouterLink>> links = LocalRouterLink::CreatePair(routers);
  routers.left()->SetOutwardLink(std::move(links.left()));
  routers.right()->SetOutwardLink(std::move(links.right()));
  return {mem::MakeRefCounted<Portal>(node, std::move(routers.left())),
          mem::MakeRefCounted<Portal>(node, std::move(routers.right()))};
}

IpczResult Portal::Close() {
  router_->CloseRoute();
  return IPCZ_RESULT_OK;
}

IpczResult Portal::QueryStatus(IpczPortalStatus& status) {
  router_->QueryStatus(status);
  return IPCZ_RESULT_OK;
}

IpczResult Portal::Put(absl::Span<const uint8_t> data,
                       absl::Span<const IpczHandle> portal_handles,
                       absl::Span<const IpczOSHandle> os_handles,
                       const IpczPutLimits* limits) {
  Parcel::PortalVector portals;
  if (!ValidateAndAcquirePortalsForTransitFrom(*this, portal_handles,
                                               portals)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (limits &&
      router_->WouldOutboundParcelExceedLimits(data.size(), *limits)) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  if (router_->IsPeerClosed()) {
    return IPCZ_RESULT_NOT_FOUND;
  }

  absl::MutexLock lock(&mutex_);
  if (in_two_phase_put_) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  std::vector<os::Handle> handles(os_handles.size());
  for (size_t i = 0; i < os_handles.size(); ++i) {
    handles[i] = os::Handle::FromIpczOSHandle(os_handles[i]);
  }

  IpczResult result = router_->SendOutboundParcel(data, portals, handles);
  if (result == IPCZ_RESULT_OK) {
    // If the parcel was sent, the sender relinquished handle ownership and
    // therefore implicitly releases its ref to each portal.
    for (IpczHandle handle : portal_handles) {
      mem::Ref<Portal> released_portal(mem::RefCounted::kAdoptExistingRef,
                                       ToPtr<Portal>(handle));
    }
  } else {
    for (os::Handle& handle : handles) {
      (void)handle.release();
    }
  }

  return result;
}

IpczResult Portal::BeginPut(IpczBeginPutFlags flags,
                            const IpczPutLimits* limits,
                            uint32_t& num_data_bytes,
                            void** data) {
  if (limits &&
      router_->WouldOutboundParcelExceedLimits(num_data_bytes, *limits)) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  if (router_->IsPeerClosed()) {
    return IPCZ_RESULT_NOT_FOUND;
  }

  absl::MutexLock lock(&mutex_);
  if (in_two_phase_put_) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  in_two_phase_put_ = true;
  pending_parcel_.emplace();
  pending_parcel_->ResizeData(num_data_bytes);
  if (data) {
    *data = pending_parcel_->data_view().data();
  }
  return IPCZ_RESULT_OK;
}

IpczResult Portal::CommitPut(uint32_t num_data_bytes_produced,
                             absl::Span<const IpczHandle> portal_handles,
                             absl::Span<const IpczOSHandle> os_handles) {
  Parcel::PortalVector portals;
  if (!ValidateAndAcquirePortalsForTransitFrom(*this, portal_handles,
                                               portals)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  Parcel parcel;
  {
    absl::MutexLock lock(&mutex_);
    if (!in_two_phase_put_ || !pending_parcel_) {
      return IPCZ_RESULT_FAILED_PRECONDITION;
    }

    if (num_data_bytes_produced > pending_parcel_->data_view().size()) {
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    parcel = std::move(*pending_parcel_);
    pending_parcel_.reset();
  }

  std::vector<os::Handle> handles(os_handles.size());
  for (size_t i = 0; i < os_handles.size(); ++i) {
    handles[i] = os::Handle::FromIpczOSHandle(os_handles[i]);
  }

  IpczResult result = router_->SendOutboundParcel(
      parcel.data_view().subspan(0, num_data_bytes_produced), portals, handles);
  if (result == IPCZ_RESULT_OK) {
    // If the parcel was sent, the sender relinquished handle ownership and
    // therefore implicitly releases its ref to each portal.
    for (IpczHandle handle : portal_handles) {
      mem::Ref<Portal> released_portal(mem::RefCounted::kAdoptExistingRef,
                                       ToPtr<Portal>(handle));
    }

    absl::MutexLock lock(&mutex_);
    in_two_phase_put_ = false;
  } else {
    for (os::Handle& handle : handles) {
      (void)handle.release();
    }

    absl::MutexLock lock(&mutex_);
    pending_parcel_.emplace(std::move(parcel));
  }

  return result;
}

IpczResult Portal::AbortPut() {
  absl::MutexLock lock(&mutex_);
  if (!in_two_phase_put_) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  in_two_phase_put_ = false;
  pending_parcel_.reset();
  return IPCZ_RESULT_OK;
}

IpczResult Portal::Get(void* data,
                       uint32_t* num_data_bytes,
                       IpczHandle* portals,
                       uint32_t* num_portals,
                       IpczOSHandle* os_handles,
                       uint32_t* num_os_handles) {
  absl::MutexLock lock(&mutex_);
  if (in_two_phase_get_) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  return router_->GetNextIncomingParcel(
      data, num_data_bytes, portals, num_portals, os_handles, num_os_handles);
}

IpczResult Portal::BeginGet(const void** data,
                            uint32_t* num_data_bytes,
                            uint32_t* num_portals,
                            uint32_t* num_os_handles) {
  absl::MutexLock lock(&mutex_);
  if (in_two_phase_get_) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  if (router_->IsRouteDead()) {
    return IPCZ_RESULT_NOT_FOUND;
  }

  IpczResult result = router_->BeginGetNextIncomingParcel(
      data, num_data_bytes, num_portals, num_os_handles);
  if (result == IPCZ_RESULT_OK) {
    in_two_phase_get_ = true;
  }
  return result;
}

IpczResult Portal::CommitGet(uint32_t num_data_bytes_consumed,
                             IpczHandle* portals,
                             uint32_t* num_portals,
                             IpczOSHandle* os_handles,
                             uint32_t* num_os_handles) {
  absl::MutexLock lock(&mutex_);
  if (!in_two_phase_get_) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  IpczResult result = router_->CommitGetNextIncomingParcel(
      num_data_bytes_consumed, portals, num_portals, os_handles,
      num_os_handles);
  if (result == IPCZ_RESULT_OK) {
    in_two_phase_get_ = false;
  }
  return result;
}

IpczResult Portal::AbortGet() {
  absl::MutexLock lock(&mutex_);
  if (!in_two_phase_get_) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  in_two_phase_get_ = false;
  return IPCZ_RESULT_OK;
}

IpczResult Portal::CreateTrap(const IpczTrapConditions& conditions,
                              IpczTrapEventHandler handler,
                              uintptr_t context,
                              IpczHandle& trap) {
  auto new_trap = std::make_unique<Trap>(conditions, handler, context);
  trap = ToHandle(new_trap.get());
  return router_->AddTrap(std::move(new_trap));
}

IpczResult Portal::ArmTrap(IpczHandle trap,
                           IpczTrapConditionFlags* satisfied_condition_flags,
                           IpczPortalStatus* status) {
  IpczTrapConditionFlags flags = 0;
  IpczResult result = router_->ArmTrap(ToRef<Trap>(trap), flags, status);
  if (result == IPCZ_RESULT_OK) {
    return IPCZ_RESULT_OK;
  }

  if (satisfied_condition_flags) {
    *satisfied_condition_flags = flags;
  }
  return result;
}

IpczResult Portal::DestroyTrap(IpczHandle trap) {
  return router_->RemoveTrap(ToRef<Trap>(trap));
}

}  // namespace core
}  // namespace ipcz
