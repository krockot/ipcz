// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/portal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "ipcz/api_object.h"
#include "ipcz/ipcz.h"
#include "ipcz/link_type.h"
#include "ipcz/local_router_link.h"
#include "ipcz/node.h"
#include "ipcz/parcel.h"
#include "ipcz/router.h"
#include "ipcz/router_link_state.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/log.h"
#include "util/ref_counted.h"

namespace ipcz {

namespace {

bool ValidateAndAcquireObjectsForTransitFrom(
    Portal& sender,
    absl::Span<const IpczHandle> handles,
    std::vector<Ref<APIObject>>& objects) {
  objects.resize(handles.size());
  for (size_t i = 0; i < handles.size(); ++i) {
    auto* object = APIObject::FromHandle(handles[i]);
    if (!object || !object->CanSendFrom(sender)) {
      return false;
    }
    objects[i] = WrapRefCounted(object);
  }
  return true;
}

}  // namespace

Portal::Portal(Ref<Node> node, Ref<Router> router)
    : node_(std::move(node)), router_(std::move(router)) {}

Portal::~Portal() = default;

// static
Portal::Pair Portal::CreatePair(Ref<Node> node) {
  Router::Pair routers{MakeRefCounted<Router>(), MakeRefCounted<Router>()};
  DVLOG(5) << "Created new portal pair with routers " << routers.first.get()
           << " and " << routers.second.get();

  RouterLink::Pair links = LocalRouterLink::CreatePair(
      LinkType::kCentral, LocalRouterLink::InitialState::kCanBypass, routers);
  routers.first->SetOutwardLink(std::move(links.first));
  routers.second->SetOutwardLink(std::move(links.second));
  return {MakeRefCounted<Portal>(node, std::move(routers.first)),
          MakeRefCounted<Portal>(node, std::move(routers.second))};
}

IpczResult Portal::Close() {
  router_->CloseRoute();
  return IPCZ_RESULT_OK;
}

bool Portal::CanSendFrom(Portal& sender) {
  return &sender != this && !sender.router()->HasLocalPeer(router_);
}

IpczResult Portal::Merge(Portal& other) {
  return router_->Merge(other.router());
}

IpczResult Portal::QueryStatus(IpczPortalStatus& status) {
  router_->QueryStatus(status);
  return IPCZ_RESULT_OK;
}

IpczResult Portal::Put(absl::Span<const uint8_t> data,
                       absl::Span<const IpczHandle> handles,
                       const IpczPutLimits* limits) {
  std::vector<Ref<APIObject>> objects;
  if (!ValidateAndAcquireObjectsForTransitFrom(*this, handles, objects)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (limits &&
      router_->WouldOutboundParcelExceedLimits(data.size(), *limits, nullptr)) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  if (router_->IsPeerClosed()) {
    return IPCZ_RESULT_NOT_FOUND;
  }

  Parcel parcel;
  IpczResult result = router_->AllocateOutboundParcel(
      data.size(), /*allow_partial=*/false, parcel);
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  memcpy(parcel.data_view().data(), data.data(), data.size());
  parcel.SetDataSize(data.size());
  parcel.SetObjects(std::move(objects));
  result = router_->SendOutboundParcel(parcel);
  if (result == IPCZ_RESULT_OK) {
    // If the parcel was sent, the sender relinquishes handle ownership and
    // therefore implicitly releases its ref to each object.
    for (IpczHandle handle : handles) {
      APIObject::TakeFromHandle(handle);
    }
  }

  return result;
}

IpczResult Portal::BeginPut(IpczBeginPutFlags flags,
                            const IpczPutLimits* limits,
                            uint32_t& num_data_bytes,
                            void** data) {
  const bool allow_partial = (flags & IPCZ_BEGIN_PUT_ALLOW_PARTIAL) != 0;
  size_t max_data_size = 0;
  if (limits && router_->WouldOutboundParcelExceedLimits(
                    num_data_bytes, *limits, &max_data_size)) {
    if (!allow_partial || max_data_size == 0) {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
    }

    num_data_bytes = max_data_size;
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

  const IpczResult result = router_->AllocateOutboundParcel(
      num_data_bytes, allow_partial, *pending_parcel_);
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  num_data_bytes = static_cast<uint32_t>(pending_parcel_->data_view().size());
  if (data) {
    *data = pending_parcel_->data_view().data();
  }
  return IPCZ_RESULT_OK;
}

IpczResult Portal::CommitPut(uint32_t num_data_bytes_produced,
                             absl::Span<const IpczHandle> handles) {
  std::vector<Ref<APIObject>> objects;
  if (!ValidateAndAcquireObjectsForTransitFrom(*this, handles, objects)) {
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

  parcel.SetDataSize(num_data_bytes_produced);
  parcel.SetObjects(std::move(objects));
  IpczResult result = router_->SendOutboundParcel(parcel);
  if (result == IPCZ_RESULT_OK) {
    // If the parcel was sent, the sender relinquishes handle ownership and
    // therefore implicitly releases its ref to each object.
    for (IpczHandle handle : handles) {
      APIObject::TakeFromHandle(handle);
    }

    absl::MutexLock lock(&mutex_);
    in_two_phase_put_ = false;
  } else {
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

IpczResult Portal::Get(IpczGetFlags flags,
                       void* data,
                       uint32_t* num_data_bytes,
                       IpczHandle* handles,
                       uint32_t* num_handles) {
  absl::MutexLock lock(&mutex_);
  if (in_two_phase_get_) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  return router_->GetNextIncomingParcel(flags, data, num_data_bytes, handles,
                                        num_handles);
}

IpczResult Portal::BeginGet(const void** data,
                            uint32_t* num_data_bytes,
                            uint32_t* num_handles) {
  absl::MutexLock lock(&mutex_);
  if (in_two_phase_get_) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  if (router_->IsRouteDead()) {
    return IPCZ_RESULT_NOT_FOUND;
  }

  IpczResult result =
      router_->BeginGetNextIncomingParcel(data, num_data_bytes, num_handles);
  if (result == IPCZ_RESULT_OK) {
    in_two_phase_get_ = true;
  }
  return result;
}

IpczResult Portal::CommitGet(uint32_t num_data_bytes_consumed,
                             absl::Span<IpczHandle> handles) {
  absl::MutexLock lock(&mutex_);
  if (!in_two_phase_get_) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  IpczResult result =
      router_->CommitGetNextIncomingParcel(num_data_bytes_consumed, handles);
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

}  // namespace ipcz
