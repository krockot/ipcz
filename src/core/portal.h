// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_H_
#define IPCZ_SRC_CORE_PORTAL_H_

#include <cstdint>
#include <utility>

#include "core/name.h"
#include "core/node.h"
#include "core/side.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class NodeLink;
class Parcel;
class PortalBackend;
class TrapEventDispatcher;

class Portal;

// Helper for acquiring a Portal reference with the Portals' internal lock held,
// along with a reference to its local peer Portal if applicable, also with its
// internal lock held. This is useful when preparing a local portal for travel
// through a buffering or routed portal, since local portal pairs need to be
// split atomically in such cases.
class LockedPortal {
 public:
  explicit LockedPortal(Portal& portal);
  ~LockedPortal();

  Portal& portal() { return *portal_; }
  Portal* peer() { return peer_.get(); }

 private:
  const mem::Ref<Portal> portal_;
  mem::Ref<Portal> peer_;
};

// Shared memory state passed to wherever a portal is moving, used to
// synchronize information between the original portal location and its new
// location.
struct PortalInTransitState {
  PortalName new_name;
};

struct PortalInTransit {
  PortalInTransit();
  PortalInTransit(PortalInTransit&&);
  PortalInTransit& operator=(PortalInTransit&&);
  ~PortalInTransit();

  mem::Ref<Portal> portal;
  Side side;

  // The portal's routed name on the sender's node. If the portal was never a
  // routed portal, a new random name will be assigned.
  PortalName local_name;

  // The portal's new routed name on the destination node.
  PortalName new_name;

  // The backend in use prior to preparation for transit. Saved in case we
  // cancel and want to restore the portal to its original state.
  std::unique_ptr<PortalBackend> backend_before_transit;

  // Same as above but for the portal's peer. This is in case the portal was
  // part of a local pair which we may want to restore upon travel cancellation.
  mem::Ref<Portal> peer_before_transit;
  std::unique_ptr<PortalBackend> peer_backend_before_transit;

  // The name of the portal before it was moved. May be null if the portal was
  // never routed to.
  absl::optional<PortalName> local_routed_name;

  // Shared memory and corresponding mapping for PortalInTransitState to be
  // shared between the original Portal and its move destination.
  os::Memory in_transit_state_memory;
  os::Memory::Mapping in_transit_state_mapping;
};

class Portal : public mem::RefCounted {
 public:
  enum { kNonTransferrable };

  using Pair = std::pair<mem::Ref<Portal>, mem::Ref<Portal>>;

  explicit Portal(Node& node);
  Portal(Node& node, std::unique_ptr<PortalBackend> backend);
  Portal(Node& node,
         std::unique_ptr<PortalBackend> backend,
         decltype(kNonTransferrable));

  static Pair CreateLocalPair(Node& node);

  std::unique_ptr<PortalBackend> TakeBackend();
  void SetBackend(std::unique_ptr<PortalBackend> backend);

  // Transitions from buffering to routing.
  bool StartRouting(const PortalName& my_name,
                    mem::Ref<NodeLink> link,
                    const PortalName& remote_portal,
                    os::Memory::Mapping control_block_mapping);

  // Accepts a parcel from an external source, e.g. as routed from another node.
  bool AcceptParcel(Parcel& parcel, TrapEventDispatcher& dispatcher);

  bool NotifyPeerClosed(TrapEventDispatcher& dispatcher);

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
  friend class LockedPortal;

  Portal(Node& node,
         std::unique_ptr<PortalBackend> backend,
         bool transferrable);
  ~Portal() override;

  bool ValidatePortalsForTravelFromHere(
      absl::Span<PortalInTransit> portals_in_transit);
  static void PreparePortalsForTravel(
      absl::Span<PortalInTransit> portals_in_transit);
  static void RestorePortalsFromCancelledTravel(
      absl::Span<PortalInTransit> portals_in_transit);
  static void FinalizePortalsAfterTravel(absl::Span<const IpczHandle> portals);

  void PrepareForTravel(PortalInTransit& portal_in_transit);
  void RestoreFromCancelledTravel(PortalInTransit& portal_in_transit);
  void FinalizeAfterTravel();

  const mem::Ref<Node> node_;
  const bool transferrable_;

  absl::Mutex mutex_;
  std::unique_ptr<PortalBackend> backend_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_H_
