// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_H_
#define IPCZ_SRC_CORE_PORTAL_H_

#include <cstdint>
#include <utility>

#include "core/incoming_parcel_queue.h"
#include "core/node.h"
#include "core/outgoing_parcel_queue.h"
#include "core/parcel.h"
#include "core/portal_link.h"
#include "core/routing_id.h"
#include "core/routing_mode.h"
#include "core/sequence_number.h"
#include "core/side.h"
#include "core/trap.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/base/thread_annotations.h"
#include "third_party/abseil-cpp/absl/numeric/int128.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class NodeLink;
class TrapEventDispatcher;
struct PortalDescriptor;

class Portal : public mem::RefCounted {
 public:
  using Pair = std::pair<mem::Ref<Portal>, mem::Ref<Portal>>;

  Portal(mem::Ref<Node> node, Side side);

  static Pair CreateLocalPair(mem::Ref<Node> node);

  Side side() const { return side_; }

  // Serializes this portal for transfer to another node. `descriptor` must be
  // filled in by the Portal. This returns the local Portal object which should
  // should immediately begin receiving requests from the destination node once
  // the transfer happens. Typically `this`, but if this was part of a local
  // pair it may be `local_peer_` instead.
  mem::Ref<Portal> Serialize(PortalDescriptor& descriptor);

  // Deserializes a new Portal from a descriptor and link state received from
  // `from_node`.
  static mem::Ref<Portal> DeserializeNew(const mem::Ref<Node>& on_node,
                                         const mem::Ref<NodeLink>& from_node,
                                         os::Memory::Mapping link_state_mapping,
                                         const PortalDescriptor& descriptor);

  // Endows a buffering portal with a new PortalLink to use as its peer link.
  // If the portal has a successor this immediately puts the portal into
  // half-proxying mode. Returns the SequenceNumber of the first non-buffered
  // outgoing message.
  SequenceNumber ActivateFromBuffering(mem::Ref<PortalLink> peer);

  // TODO: document this
  void BeginForwardProxying(mem::Ref<PortalLink> successor);

  // Accepts a parcel that was routed here by a NodeLink. If this parcel receipt
  // triggers any trap events, they'll be added to `dispatcher` for imminent
  // dispatch.
  bool AcceptParcelFromLink(NodeLink& link,
                            RoutingId routing_id,
                            Parcel& parcel,
                            TrapEventDispatcher& dispatcher);

  // Invoked to notify this portal that one side of its route has been closed.
  // If this triggers any trap events, they'll be added to `dispatcher` for
  // imminent dispatch.
  bool OnSideClosed(Side side,
                    SequenceNumber sequence_length,
                    TrapEventDispatcher& dispatcher);

  // Attempts to replace the portal's current peer link with a new one. The
  // given `bypass_key` must match the `bypass_key` stored in the current peer
  // link's PortalLinkState. This operation is valid whether or not the portal
  // had a peer link already.
  bool ReplacePeerLink(absl::uint128 bypass_key,
                       const mem::Ref<PortalLink>& new_peer);

  // Iff this portal is in half-proxy mode, it can expect no parcels with a
  // SequenceNumber of `sequence_length` or higher; so if it has already seen
  // and forwarded messages below that number, it can disappear.
  bool StopProxyingTowardSide(NodeLink& from_node,
                              RoutingId from_routing_id,
                              Side side,
                              SequenceNumber sequence_length);

  // Initiates the removal of a proxying predecessor to this portal by
  // contacting the predecessor's peer with a request to bypass the predecessor
  // and route directly to this portal instead. `notify_predecessor` is true
  // only when we have a half-proxying predecessor which was previously a full
  // proxy. In any other case, the predecessor has no need to be notified.
  bool InitiateProxyBypass(NodeLink& requesting_node,
                           RoutingId requesting_routing_id,
                           const NodeName& peer_name,
                           RoutingId peer_proxy_routing_id,
                           absl::uint128 bypass_key,
                           bool notify_predecessor);

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

  // Locks two portals' Mutexes in a globally consistent order.
  class ABSL_SCOPED_LOCKABLE TwoPortalLock {
   public:
    TwoPortalLock(Portal& a, Portal& b)
        ABSL_EXCLUSIVE_LOCK_FUNCTION(&a.mutex_, &b.mutex_);
    ~TwoPortalLock() ABSL_UNLOCK_FUNCTION();

   private:
    Portal& a_;
    Portal& b_;
  };

  ~Portal() override;

  bool Deserialize(const mem::Ref<NodeLink>& from_node,
                   os::Memory::Mapping link_state_mapping,
                   const PortalDescriptor& descriptor);

  bool ValidatePortalsToSendFromHere(absl::Span<mem::Ref<Portal>> portals);

  IpczResult ValidatePutLimits(size_t data_size, const IpczPutLimits* limits);
  IpczResult PutImpl(absl::Span<const uint8_t> data,
                     Parcel::PortalVector& portals,
                     std::vector<os::Handle>& os_handles,
                     const IpczPutLimits* limits,
                     bool is_two_phase_commit);

  void ForwardParcels();

  const mem::Ref<Node> node_;
  const Side side_;

  absl::Mutex mutex_;

  // Local cache of our current RoutingMode.
  RoutingMode routing_mode_ = RoutingMode::kBuffering;

  // Non-null if and only if this portal's peer is local to the same node. In
  // this case both portals are always locked together by any PortalLock
  // guarding access to all the state below, and operations on one portal may
  // directly manipulate the state of the other.
  mem::Ref<Portal> local_peer_ ABSL_GUARDED_BY(mutex_);

  // `peer_link_` is non-null if and only if this Portal may actively transmit
  // its outgoing parcels to a known peer on another node.
  mem::Ref<PortalLink> peer_link_ ABSL_GUARDED_BY(mutex_);

  mem::Ref<PortalLink> predecessor_link_ ABSL_GUARDED_BY(mutex_);

  // `successor_link_` is non-null if and only iff this Portal has been moved
  // and is still hanging around to forward one or more expected incoming
  // parcels to its successor.
  mem::Ref<PortalLink> successor_link_ ABSL_GUARDED_BY(mutex_);

  // Tracks whether this portal has been explicitly closed. Closed portals are
  // not meant to be used further by any APIs.
  bool closed_ ABSL_GUARDED_BY(mutex_) = false;

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
  IncomingParcelQueue incoming_parcels_ ABSL_GUARDED_BY(mutex_);

  // Queue of outgoing parcels to be sent out if and when the portal is given an
  // active link. Note that this queue is *always* empty if the portal's peer is
  // local, because in such cases, Put() operations directly manipulate the peer
  // portal's `incoming_parcels_` queue.
  OutgoingParcelQueue outgoing_parcels_ ABSL_GUARDED_BY(mutex_);

  // An incoming queue of outgoing parcels to be fowarded to our peer or
  // predecessor. Present if and only if we're a half-proxy that decayed from a
  // full proxy.
  absl::optional<IncomingParcelQueue> outgoing_parcels_from_successor_
      ABSL_GUARDED_BY(mutex_);

  // The next SequenceNumber to use for any outgoing parcel.
  SequenceNumber next_outgoing_sequence_number_ ABSL_GUARDED_BY(mutex_) = 0;

  // The set of traps attached to this portal.
  TrapSet traps_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_H_
