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
#include "core/route_id.h"
#include "core/sequence_number.h"
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
struct PortalDescriptor;

class Portal : public mem::RefCounted {
 public:
  enum { kNonTransferrable };

  using Pair = std::pair<mem::Ref<Portal>, mem::Ref<Portal>>;

  explicit Portal(Side side);
  Portal(Side side, decltype(kNonTransferrable));

  static Pair CreateLocalPair(Node& node);

  Side side() const { return side_; }

  // Serializes this portal for transfer to another node. `descriptor` must be
  // filled in by the Portal. This returns the local Portal object which should
  // should immediately begin receiving requests from the destination node once
  // the transfer happens. Typically `this`, but if this was part of a local
  // pair it may be `local_peer_` instead.
  mem::Ref<Portal> Serialize(PortalDescriptor& descriptor);

  // Deserializes a new Portal from a descriptor and link state received from
  // `from_node`.
  static mem::Ref<Portal> DeserializeNew(NodeLink& from_node,
                                         os::Memory::Mapping link_state_mapping,
                                         const PortalDescriptor& descriptor);

  void SetPeerLink(mem::Ref<PortalLink> link);
  void SetSuccessorLink(mem::Ref<PortalLink> link);

  // Accepts a parcel that was routed here by a NodeLink. If this parcel receipt
  // triggers any trap events, they'll be added to `dispatcher` for imminent
  // dispatch.
  bool AcceptParcelFromLink(Parcel& parcel, TrapEventDispatcher& dispatcher);

  // Notifies this portal that its peer has been closed. If this change triggers
  // any trap events, they'll be added to `dispatcher` for imminent dispatch.
  bool NotifyPeerClosed(SequenceNumber sequence_length,
                        TrapEventDispatcher& dispatcher);

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

  Portal(Side side, bool transferrable);
  ~Portal() override;

  bool Deserialize(NodeLink& from_node,
                   os::Memory::Mapping link_state_mapping,
                   const PortalDescriptor& descriptor);

  bool ValidatePortalsToSendFromHere(absl::Span<mem::Ref<Portal>> portals);

  IpczResult ValidatePutLimits(size_t data_size, const IpczPutLimits* limits);
  IpczResult PutImpl(absl::Span<const uint8_t> data,
                     Parcel::PortalVector& portals,
                     std::vector<os::Handle>& os_handles,
                     const IpczPutLimits* limits,
                     bool is_two_phase_commit);
  void SendParcelOnLink(PortalLink& link, Parcel& parcel);

  const Side side_;
  const bool transferrable_;

  absl::Mutex mutex_;

  // Non-null if and only if this portal's peer is local to the same node. In
  // this case both portals are always locked together by any PortalLock
  // guarding access to all the state below, and operations on one portal may
  // directly manipulate the state of the other.
  mem::Ref<Portal> local_peer_ ABSL_GUARDED_BY(mutex_);

  // `peer_link_` is non-null if and only if this Portal may actively transmit
  // its outgoing parcels to a known peer on another node.
  mem::Ref<PortalLink> peer_link_ ABSL_GUARDED_BY(mutex_);

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

  // The next SequenceNumber to use for any outgoing parcel.
  SequenceNumber next_outgoing_sequence_number_ ABSL_GUARDED_BY(mutex_) = 0;

  // The set of traps attached to this portal.
  TrapSet traps_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_H_
