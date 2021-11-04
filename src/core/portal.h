// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_H_
#define IPCZ_SRC_CORE_PORTAL_H_

#include <cstdint>
#include <utility>

#include "core/node.h"
#include "core/parcel.h"
#include "core/parcel_queue.h"
#include "core/portal_in_transit.h"
#include "core/route_id.h"
#include "core/side.h"
#include "core/trap.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/base/thread_annotations.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class NodeLink;
class TrapEventDispatcher;

class Portal : public mem::RefCounted {
 public:
  enum { kNonTransferrable };

  using Pair = std::pair<mem::Ref<Portal>, mem::Ref<Portal>>;

  explicit Portal(Side side);
  Portal(Side side, decltype(kNonTransferrable));

  static Pair CreateLocalPair(Node& node);

  Side side() const { return side_; }

  void SetPeerLink(mem::Ref<NodeLink> link,
                   RouteId route,
                   os::Memory::Mapping control_block_mapping);
  void SetForwardingLink(mem::Ref<NodeLink> link,
                         RouteId route,
                         os::Memory::Mapping control_block_mapping);

  // Accepts a parcel that was routed here by a NodeLink. If this parcel receipt
  // triggers any trap events, they'll be added to `dispatcher` for imminent
  // dispatch.
  bool AcceptParcelFromLink(Parcel& parcel, TrapEventDispatcher& dispatcher);

  // Notifies this portal that its peer has been closed. If this change triggers
  // any trap events, they'll be added to `dispatcher` for imminent dispatch.
  bool NotifyPeerClosed(TrapEventDispatcher& dispatcher);

  // ipcz portal API implementation:
  IpczResult Close();
  IpczResult QueryStatus(IpczPortalStatus& status);

  IpczResult Put(absl::Span<const uint8_t> data,
                 absl::Span<const IpczHandle> portals,
                 absl::Span<const IpczOSHandle> os_handles,
                 const IpczPutLimits* limits);
  IpczResult BeginPut(IpczBeginPutFlags flags,
                      const IpczPutLimits* limits,
                      uint32_t& num_data_bytes,
                      void** data);
  IpczResult CommitPut(uint32_t num_data_bytes_produced,
                       absl::Span<const IpczHandle> portals,
                       absl::Span<const IpczOSHandle> os_handles);
  IpczResult AbortPut();

  IpczResult Get(void* data,
                 uint32_t* num_data_bytes,
                 IpczHandle* portals,
                 uint32_t* num_portals,
                 IpczOSHandle* os_handles,
                 uint32_t* num_os_handles);
  IpczResult BeginGet(const void** data,
                      uint32_t* num_data_bytes,
                      uint32_t* num_portals,
                      uint32_t* num_os_handles);
  IpczResult CommitGet(uint32_t num_data_bytes_consumed,
                       IpczHandle* portals,
                       uint32_t* num_portals,
                       IpczOSHandle* os_handles,
                       uint32_t* num_os_handles);
  IpczResult AbortGet();

  IpczResult CreateTrap(const IpczTrapConditions& conditions,
                        IpczTrapEventHandler handler,
                        uintptr_t context,
                        IpczHandle& trap);
  IpczResult ArmTrap(IpczHandle trap,
                     IpczTrapConditionFlags* satisfied_condition_flags,
                     IpczPortalStatus* status);
  IpczResult DestroyTrap(IpczHandle trap);

 private:
  // Locks this portal's Mutex and if present its local peer's Mutex, with a
  // globally consistent ordering to avoid lock-order inversion.
  class ABSL_SCOPED_LOCKABLE PortalLock {
   public:
    explicit PortalLock(Portal& portal)
        ABSL_EXCLUSIVE_LOCK_FUNCTION(&portal.mutex_,
                                     &portal.local_peer_->mutex_);
    ~PortalLock() ABSL_UNLOCK_FUNCTION();

   private:
    Portal& portal_;
    mem::Ref<Portal> locked_peer_;
  };

  Portal(Side side, bool transferrable);
  ~Portal() override;

  bool ValidatePortalsForTransitFromHere(
      absl::Span<PortalInTransit> portals_in_transit);
  static void PreparePortalsForTransit(
      absl::Span<PortalInTransit> portals_in_transit);
  static void RestorePortalsFromCancelledTransit(
      absl::Span<PortalInTransit> portals_in_transit);
  static void FinalizePortalsAfterTransit(absl::Span<PortalInTransit> portals);

  void PrepareForTransit(PortalInTransit& portal_in_transit);
  void RestoreFromCancelledTransit(PortalInTransit& portal_in_transit);
  void FinalizeAfterTransit(PortalInTransit& portal_in_transit);

  IpczResult ValidatePutLimits(size_t data_size, const IpczPutLimits* limits);
  IpczResult PutImpl(absl::Span<const uint8_t> data,
                     Parcel::PortalVector& portals,
                     std::vector<os::Handle>& os_handles,
                     const IpczPutLimits* limits,
                     bool is_two_phase_commit);

  const Side side_;
  const bool transferrable_;

  absl::Mutex mutex_;

  // Non-null if and only if this portal's peer is local to the same node. In
  // this case both portals are always locked together by any PortalLock
  // guarding access to all the state below, and operations on one portal may
  // directly manipulate the state of the other.
  mem::Ref<Portal> local_peer_ ABSL_GUARDED_BY(mutex_);

  // `peer_link_` is non-null if and only if this Portal may actively transmit
  // its outgoing parcels to that NodeLink on the given `route_`. In this case,
  // the corresponding PortalControlBlock is also mapped by
  // `peer_control_block_`. When `peer_link_` is null, both `peer_route_` and
  // `peer_control_block_` are unused.
  mem::Ref<NodeLink> peer_link_ ABSL_GUARDED_BY(mutex_);
  RouteId peer_route_ ABSL_GUARDED_BY(mutex_);
  os::Memory::Mapping peer_control_block_ ABSL_GUARDED_BY(mutex_);

  // `forwarding_link_` is non-null if and only iff this Portal has been moved
  // and is still hanging around to forward one or more expected incoming
  // parcels. Such parcels when received are forwarded to `forwarding_link_` on
  // `forwarding_route_`, and the corresponding PortalControlBlock is mapped by
  // `forwarding_control_block_`. When `forwarding_link_` is null, both
  // `forwarding_route_` and `forwarding_control_block_` are unused.
  //
  // TODO: This collection of state is suspiciously like the peer state above.
  // Probably there's a common abstraction to cut out of this. (a PortalLink?)
  mem::Ref<NodeLink> forwarding_link_ ABSL_GUARDED_BY(mutex_);
  RouteId forwarding_route_ ABSL_GUARDED_BY(mutex_);
  os::Memory::Mapping forwarding_control_block_ ABSL_GUARDED_BY(mutex_);

  // Tracks whether this portal has been explicitly closed. Closed portals are
  // not meant to be used further by any APIs.
  bool closed_ ABSL_GUARDED_BY(mutex_) = false;

  // Tracks whether this portal has been moved elsewhere. In this case the
  // portal may persist with a peer link for forwarding messages to the new
  // location, and potentially multiple inbound routes receiving messages the
  // peer or prior locations. Eventually the portal will be destroyed once it
  // has finished forwarding what it expects to have forwarded through it.
  bool moved_ ABSL_GUARDED_BY(mutex_) = false;

  // The current IpczPortalStatus of this portal. The status is used frequently
  // for status queries and trap events, and in such cases it is copied directly
  // from this cached value. As such, this value must be maintained by all
  // operations that mutate relevant state on the portal or its peer.
  IpczPortalStatus status_ ABSL_GUARDED_BY(mutex_) = {sizeof(status_)};

  // If a two-phase Put operation is in progress, this is the Parcel it's
  // building.
  //
  // TODO: For data-only parcels (flag on BeginPut? flag on portal creation?)
  // we could allocate a chunk of shared memory so the parcel can be built
  // directly there.
  absl::optional<Parcel> pending_parcel_ ABSL_GUARDED_BY(mutex_);

  // If a two-phase Get operation is in progress, this guards against other Get
  // operations interrupting it.
  bool in_two_phase_get_ ABSL_GUARDED_BY(mutex_) = false;

  // Queue of incoming parcels that have yet to be read by the application. If
  // this portal has moved and receives parcels to be forwarded, those parcels
  // may also be queued here until we have an active link to forward them to.
  ParcelQueue incoming_parcels_ ABSL_GUARDED_BY(mutex_);

  // Queue of outgoing parcels to be sent out if and when the portal is given an
  // active link. Note that this queue is *always* empty if the portal's peer is
  // local, because in such cases, Put() operations directly manipulate the peer
  // portal's `incoming_parcels_` queue.
  ParcelQueue outgoing_parcels_ ABSL_GUARDED_BY(mutex_);

  // The set of traps attached to this portal.
  TrapSet traps_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_H_
