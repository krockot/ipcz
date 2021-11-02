// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/portal.h"

#include <memory>
#include <utility>

#include "core/buffering_portal_backend.h"
#include "core/direct_portal_backend.h"
#include "core/name.h"
#include "core/node.h"
#include "core/parcel.h"
#include "core/portal_backend.h"
#include "core/portal_control_block.h"
#include "core/routed_portal_backend.h"
#include "core/trap.h"
#include "debug/log.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/handle_util.h"

namespace ipcz {
namespace core {

namespace {

absl::InlinedVector<PortalInTransit, 4> AcquirePortalsForTravel(
    absl::Span<const IpczHandle> handles) {
  absl::InlinedVector<PortalInTransit, 4> portals(handles.size());
  for (size_t i = 0; i < handles.size(); ++i) {
    portals[i].portal = mem::WrapRefCounted(ToPtr<Portal>(handles[i]));
  }
  return portals;
}

}  // namespace

LockedPortal::LockedPortal(Portal& portal) : portal_(&portal) {
  portal.mutex_.Lock();
  if (portal.backend_->GetType() != PortalBackend::Type::kDirect) {
    // Definitely no local peer, so we're done locking what we need.
    return;
  }

  auto* backend = static_cast<DirectPortalBackend*>(portal.backend_.get());
  mem::Ref<Portal> original_peer = backend->GetLocalPeer();
  if (!original_peer) {
    // There is no peer. Must have been closed.
    return;
  }

  if (original_peer.get() < &portal) {
    // We need to unlock `portal` first, then re-lock after acquiring `peer`.
    // Consistently ordering mutliple portal lock acquisitions (in this case,
    // ordered by increasing Portal memory address) avoids lock-order inversion.
    portal.mutex_.Unlock();
    original_peer->mutex_.Lock();
    portal.mutex_.Lock();
  } else {
    original_peer->mutex_.Lock();
  }

  // Now both portals are locked in the correct order. Since we may have had
  // to unlock temporarily, state may have changed. First verify that we are
  // still a local pair and we still have a peer.
  if (portal.backend_->GetType() != PortalBackend::Type::kDirect) {
    // We're no longer a local pair, so nothing else to do.
    original_peer->mutex_.Unlock();
    return;
  }
  backend = static_cast<DirectPortalBackend*>(portal.backend_.get());
  mem::Ref<Portal> current_peer = backend->GetLocalPeer();
  if (!current_peer) {
    // Peer was closed since we started this process. Let it go.
    original_peer->mutex_.Unlock();
    return;
  }

  // Local portal pairs do not ever change their peer while keeping the same
  // backend. If the peer changed, either it was closed and became null, or the
  // portal already switched to a different backend type and we wouldn't end up
  // here.
  ABSL_ASSERT(current_peer == original_peer);
  peer_ = std::move(current_peer);
}

LockedPortal::~LockedPortal() {
  if (peer_) {
    peer_->mutex_.AssertHeld();
    peer_->mutex_.Unlock();
  }
  portal_->mutex_.AssertHeld();
  portal_->mutex_.Unlock();
}

PortalInTransit::PortalInTransit() = default;

PortalInTransit::PortalInTransit(PortalInTransit&&) = default;

PortalInTransit& PortalInTransit::operator=(PortalInTransit&&) = default;

PortalInTransit::~PortalInTransit() = default;

Portal::Portal(Node& node) : Portal(node, nullptr, /*transferrable=*/true) {}

Portal::Portal(Node& node, std::unique_ptr<PortalBackend> backend)
    : Portal(node, std::move(backend), /*transferrable=*/true) {}

Portal::Portal(Node& node,
               std::unique_ptr<PortalBackend> backend,
               decltype(kNonTransferrable))
    : Portal(node, std::move(backend), /*transferrable=*/false) {}

Portal::Portal(Node& node,
               std::unique_ptr<PortalBackend> backend,
               bool transferrable)
    : node_(mem::WrapRefCounted(&node)),
      transferrable_(transferrable),
      backend_(std::move(backend)) {}

Portal::~Portal() = default;

// static
Portal::Pair Portal::CreateLocalPair(Node& node) {
  auto portal0 = mem::MakeRefCounted<Portal>(node);
  auto portal1 = mem::MakeRefCounted<Portal>(node);
  DirectPortalBackend::Pair backends =
      DirectPortalBackend::CreatePair(*portal0, *portal1);
  portal0->SetBackend(std::move(backends.first));
  portal1->SetBackend(std::move(backends.second));
  return {std::move(portal0), std::move(portal1)};
}

std::unique_ptr<PortalBackend> Portal::TakeBackend() {
  absl::MutexLock lock(&mutex_);
  return std::move(backend_);
}

void Portal::SetBackend(std::unique_ptr<PortalBackend> backend) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!backend_);
  backend_ = std::move(backend);
}

bool Portal::StartRouting(const PortalName& my_name,
                          mem::Ref<NodeLink> link,
                          const PortalName& remote_portal,
                          os::Memory::Mapping control_block_mapping) {
  absl::MutexLock lock(&mutex_);
  if (!backend_ || backend_->GetType() != PortalBackend::Type::kBuffering) {
    return false;
  }

  auto& backend = reinterpret_cast<BufferingPortalBackend&>(*backend_);
  auto new_backend = std::make_unique<RoutedPortalBackend>(
      my_name, link, remote_portal, backend.side(),
      std::move(control_block_mapping));

  // If adoption fails, the peer has already moved and we stay in a buffering
  // state.
  //
  // TODO: use the broker to settle portal locations in cases like this.
  if (new_backend->AdoptBufferingBackendState(backend)) {
    backend_ = std::move(new_backend);
  }
  return true;
}

bool Portal::AcceptParcel(Parcel& parcel, TrapEventDispatcher& dispatcher) {
  absl::MutexLock lock(&mutex_);
  return backend_->AcceptParcel(parcel, dispatcher);
}

bool Portal::NotifyPeerClosed(TrapEventDispatcher& dispatcher) {
  absl::MutexLock lock(&mutex_);
  return backend_->NotifyPeerClosed(dispatcher);
}

IpczResult Portal::Close() {
  std::vector<mem::Ref<Portal>> other_portals_to_close;
  {
    absl::MutexLock lock(&mutex_);
    IpczResult result = backend_->Close(other_portals_to_close);
    if (result != IPCZ_RESULT_OK) {
      return result;
    }
  }

  for (mem::Ref<Portal>& portal : other_portals_to_close) {
    portal->Close();
  }
  return IPCZ_RESULT_OK;
}

IpczResult Portal::QueryStatus(IpczPortalStatus& status) {
  absl::MutexLock lock(&mutex_);
  return backend_->QueryStatus(status);
}

IpczResult Portal::Put(absl::Span<const uint8_t> data,
                       absl::Span<const IpczHandle> portals,
                       absl::Span<const IpczOSHandle> os_handles,
                       const IpczPutLimits* limits) {
  auto portals_in_transit = AcquirePortalsForTravel(portals);
  auto portals_view = absl::MakeSpan(portals_in_transit);

  if (!ValidatePortalsForTravelFromHere(portals_view)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  IpczResult result;
  {
    absl::MutexLock lock(&mutex_);
    if (backend_->GetType() == PortalBackend::Type::kDirect) {
      // Fast path: direct portals don't need to do any preparation for the
      // portals they transfer.
      return backend_->Put(data, portals_view, os_handles, limits);
    }
  }

  PreparePortalsForTravel(portals_view);
  {
    absl::MutexLock lock(&mutex_);
    result = backend_->Put(data, portals_view, os_handles, limits);
    if (result == IPCZ_RESULT_UNAVAILABLE) {
      // The parcel is queued for transmission but the destination portal has
      // already moved. Switch this portal to buffering until we can figure out
      // where to send its outgoing parcels.
      ABSL_ASSERT(backend_->GetType() == PortalBackend::Type::kRouted);
      backend_ = static_cast<RoutedPortalBackend&>(*backend_).StartBuffering();
    }
  }
  if (result == IPCZ_RESULT_OK || result == IPCZ_RESULT_UNAVAILABLE) {
    FinalizePortalsAfterTravel(portals);
    return IPCZ_RESULT_OK;
  }
  RestorePortalsFromCancelledTravel(portals_view);
  return result;
}

IpczResult Portal::BeginPut(IpczBeginPutFlags flags,
                            const IpczPutLimits* limits,
                            uint32_t& num_data_bytes,
                            void** data) {
  absl::MutexLock lock(&mutex_);
  return backend_->BeginPut(flags, limits, num_data_bytes, data);
}

IpczResult Portal::CommitPut(uint32_t num_data_bytes_produced,
                             absl::Span<const IpczHandle> portals,
                             absl::Span<const IpczOSHandle> os_handles) {
  auto portals_in_transit = AcquirePortalsForTravel(portals);
  auto portals_view = absl::MakeSpan(portals_in_transit);

  if (!ValidatePortalsForTravelFromHere(portals_view)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  IpczResult result;
  {
    absl::MutexLock lock(&mutex_);
    if (backend_->GetType() == PortalBackend::Type::kDirect) {
      // Fast path: direct portals don't need to do any preparation for the
      // portals they transfer.
      return backend_->CommitPut(num_data_bytes_produced, portals_view,
                                 os_handles);
    }
  }

  PreparePortalsForTravel(portals_view);
  {
    absl::MutexLock lock(&mutex_);
    result =
        backend_->CommitPut(num_data_bytes_produced, portals_view, os_handles);
    if (result == IPCZ_RESULT_UNAVAILABLE) {
      // The parcel is queued for transmission but the destination portal has
      // already moved. Switch this portal to buffering until we can figure out
      // where to send its outgoing parcels.
      ABSL_ASSERT(backend_->GetType() == PortalBackend::Type::kRouted);
      backend_ = static_cast<RoutedPortalBackend&>(*backend_).StartBuffering();
    }
  }
  if (result == IPCZ_RESULT_OK || result == IPCZ_RESULT_UNAVAILABLE) {
    FinalizePortalsAfterTravel(portals);
    return IPCZ_RESULT_OK;
  }
  RestorePortalsFromCancelledTravel(portals_view);
  return result;
}

IpczResult Portal::AbortPut() {
  absl::MutexLock lock(&mutex_);
  return backend_->AbortPut();
}

IpczResult Portal::Get(void* data,
                       uint32_t* num_data_bytes,
                       IpczHandle* portals,
                       uint32_t* num_portals,
                       IpczOSHandle* os_handles,
                       uint32_t* num_os_handles) {
  absl::MutexLock lock(&mutex_);
  return backend_->Get(data, num_data_bytes, portals, num_portals, os_handles,
                       num_os_handles);
}

IpczResult Portal::BeginGet(const void** data,
                            uint32_t* num_data_bytes,
                            uint32_t* num_portals,
                            uint32_t* num_os_handles) {
  absl::MutexLock lock(&mutex_);
  return backend_->BeginGet(data, num_data_bytes, num_portals, num_os_handles);
}

IpczResult Portal::CommitGet(uint32_t num_data_bytes_consumed,
                             IpczHandle* portals,
                             uint32_t* num_portals,
                             IpczOSHandle* os_handles,
                             uint32_t* num_os_handles) {
  absl::MutexLock lock(&mutex_);
  return backend_->CommitGet(num_data_bytes_consumed, portals, num_portals,
                             os_handles, num_os_handles);
}

IpczResult Portal::AbortGet() {
  absl::MutexLock lock(&mutex_);
  return backend_->AbortGet();
}

IpczResult Portal::CreateTrap(const IpczTrapConditions& conditions,
                              IpczTrapEventHandler handler,
                              uintptr_t context,
                              IpczHandle& trap) {
  auto new_trap = std::make_unique<Trap>(conditions, handler, context);
  trap = ToHandle(new_trap.get());

  absl::MutexLock lock(&mutex_);
  return backend_->AddTrap(std::move(new_trap));
}

IpczResult Portal::ArmTrap(IpczHandle trap,
                           IpczTrapConditionFlags* satisfied_condition_flags,
                           IpczPortalStatus* status) {
  absl::MutexLock lock(&mutex_);
  return backend_->ArmTrap(ToRef<Trap>(trap), satisfied_condition_flags,
                           status);
}

IpczResult Portal::DestroyTrap(IpczHandle trap) {
  absl::MutexLock lock(&mutex_);
  return backend_->RemoveTrap(ToRef<Trap>(trap));
}

bool Portal::ValidatePortalsForTravelFromHere(
    absl::Span<PortalInTransit> portals_in_transit) {
  for (PortalInTransit& portal_in_transit : portals_in_transit) {
    Portal& portal = *portal_in_transit.portal;
    if (!portal.transferrable_ || &portal == this) {
      return false;
    }

    absl::MutexLock lock(&portal.mutex_);
    if (!portal.backend_->CanTravelThroughPortal(*this)) {
      return false;
    }
  }

  return true;
}

// static
void Portal::PreparePortalsForTravel(
    absl::Span<PortalInTransit> portals_in_transit) {
  for (PortalInTransit& portal : portals_in_transit) {
    ABSL_ASSERT(portal.portal);
    portal.portal->PrepareForTravel(portal);
  }
}

// static
void Portal::RestorePortalsFromCancelledTravel(
    absl::Span<PortalInTransit> portals_in_transit) {
  for (PortalInTransit& portal : portals_in_transit) {
    ABSL_ASSERT(portal.portal);
    portal.portal->RestoreFromCancelledTravel(portal);
  }
}

// static
void Portal::FinalizePortalsAfterTravel(absl::Span<const IpczHandle> portals) {
  for (IpczHandle handle : portals) {
    // Steal the handle's ref to the portal since the handle must no longer be
    // in use by the application.
    mem::Ref<Portal> portal = {mem::RefCounted::kAdoptExistingRef,
                               ToPtr<Portal>(handle)};
    portal->FinalizeAfterTravel();
  }
}

void Portal::PrepareForTravel(PortalInTransit& portal_in_transit) {
  LockedPortal locked(*this);
  mutex_.AssertHeld();
  if (backend_->GetType() == PortalBackend::Type::kBuffering) {
    // Already buffering, so there's nothing else to prepare.
    backend_->PrepareForTravel(portal_in_transit);
    return;
  }

  if (backend_->GetType() == PortalBackend::Type::kDirect) {
    // We are part of a local portal pair that now must be split across two
    // buffering backends, at least one of which will imminently be moved to
    // another node.
    //
    // Note that the peer may have already been closed, so it may be null here.
    // We still move the portal in that case since it may have unread parcels
    // to be made available at its new location.

    Portal* peer = locked.peer();
    PortalBackend* peer_backend = nullptr;
    if (peer) {
      peer->mutex_.AssertHeld();
      peer_backend = peer->backend_.get();
      ABSL_ASSERT(peer_backend->GetType() == PortalBackend::Type::kDirect);
    }

    ABSL_ASSERT(peer_backend->GetType() == PortalBackend::Type::kDirect);

    std::unique_ptr<BufferingPortalBackend> new_backend;
    std::unique_ptr<BufferingPortalBackend> new_peer_backend;
    std::tie(new_backend, new_peer_backend) = DirectPortalBackend::Split(
        static_cast<DirectPortalBackend&>(*backend_),
        static_cast<DirectPortalBackend*>(peer_backend));
    portal_in_transit.backend_before_transit = std::move(backend_);
    backend_ = std::move(new_backend);
    if (peer) {
      peer->mutex_.AssertHeld();
      ABSL_ASSERT(new_peer_backend);
      portal_in_transit.peer_before_transit = mem::WrapRefCounted(peer);
      portal_in_transit.peer_backend_before_transit = std::move(peer->backend_);
      peer->backend_ = std::move(new_peer_backend);
    }
    backend_->PrepareForTravel(portal_in_transit);
    return;
  }

  // We're currently routed. Switch to a buffering state where we may still
  // collect latent inbound parcels for forwarding. Once we have confirmation
  // that no outstanding parcels are in flight to this location, the buffering
  // portal will be destroyed.
  ABSL_ASSERT(backend_->GetType() == PortalBackend::Type::kRouted);
  auto* routed_backend = static_cast<RoutedPortalBackend*>(backend_.get());
  portal_in_transit.backend_before_transit = std::move(backend_);
  backend_ = routed_backend->StartBuffering();
  backend_->PrepareForTravel(portal_in_transit);
}

void Portal::RestoreFromCancelledTravel(PortalInTransit& portal_in_transit) {
  // TODO - not terribly important for now. mojo always discards resources that
  // were attached to messages which couldn't be sent. so in practice any
  // cancelled travel will immediately be followed by closure of all portal
  // attachments and it doesn't matter what state we leave them in. this needs
  // to be fixed eventually to meet specified ipcz API behvior though. for
  // example if a direct portal pair was spit for travel and then travel failed,
  // we should restore the direct portal pair to its original state here.
}

void Portal::FinalizeAfterTravel() {
  // TODO - plug any follow-up tracking into our Node. perhaps Node should
  // immediately track:
  //  - a link between the portal's new full PortalAddress and this Portal
  //    object
  //  - a link between this Portal object its original portal name if it was a
  //    routed portal before transmission.
  //  - information about how many parcels are expected from the pre-travel peer
  //    before it will stop sending messages to this portal altogether.
  // messages are forwarded as they're received
  // broker negotiates finalization of transfers it didn't initiate,
  // provides information about new peer location.
}

}  // namespace core
}  // namespace ipcz
