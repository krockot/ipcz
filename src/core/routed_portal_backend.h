// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTED_PORTAL_BACKEND_H_
#define IPCZ_SRC_CORE_ROUTED_PORTAL_BACKEND_H_

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "core/name.h"
#include "core/parcel.h"
#include "core/parcel_queue.h"
#include "core/portal_backend.h"
#include "core/portal_control_block.h"
#include "core/side.h"
#include "core/trap.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_set.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {
namespace core {

class BufferingPortalBackend;
class Node;

// PortalBackend implementation for a portal whose peer may live in a different
// node.
class RoutedPortalBackend : public PortalBackend {
 public:
  RoutedPortalBackend(const PortalName& name,
                      const PortalAddress& peer_address,
                      Side side,
                      os::Memory::Mapping control_block_mapping);
  ~RoutedPortalBackend() override;

  void AdoptBufferingBackendState(Node::LockedRouter& router,
                                  BufferingPortalBackend& backend);

  // PortalBackend:
  Type GetType() const override;
  bool CanTravelThroughPortal(Portal& sender) override;
  bool AcceptParcel(Parcel& parcel, TrapEventDispatcher& dispatcher) override;
  IpczResult Close(
      Node::LockedRouter& router,
      std::vector<mem::Ref<Portal>>& other_portals_to_close) override;
  IpczResult QueryStatus(IpczPortalStatus& status) override;
  IpczResult Put(Node::LockedRouter& router,
                 absl::Span<const uint8_t> data,
                 absl::Span<const IpczHandle> portals,
                 absl::Span<const IpczOSHandle> os_handles,
                 const IpczPutLimits* limits) override;
  IpczResult BeginPut(IpczBeginPutFlags flags,
                      const IpczPutLimits* limits,
                      uint32_t& num_data_bytes,
                      void** data) override;
  IpczResult CommitPut(Node::LockedRouter& router,
                       uint32_t num_data_bytes_produced,
                       absl::Span<const IpczHandle> portals,
                       absl::Span<const IpczOSHandle> os_handles) override;
  IpczResult AbortPut() override;
  IpczResult Get(void* data,
                 uint32_t* num_data_bytes,
                 IpczHandle* portals,
                 uint32_t* num_portals,
                 IpczOSHandle* os_handles,
                 uint32_t* num_os_handles) override;
  IpczResult BeginGet(const void** data,
                      uint32_t* num_data_bytes,
                      uint32_t* num_portals,
                      uint32_t* num_os_handles) override;
  IpczResult CommitGet(uint32_t num_data_bytes_consumed,
                       IpczHandle* portals,
                       uint32_t* num_portals,
                       IpczOSHandle* os_handles,
                       uint32_t* num_os_handles) override;
  IpczResult AbortGet() override;
  IpczResult AddTrap(std::unique_ptr<Trap> trap) override;
  IpczResult ArmTrap(Trap& trap,
                     IpczTrapConditionFlags* satisfied_condition_flags,
                     IpczPortalStatus* status) override;
  IpczResult RemoveTrap(Trap& trap) override;

 private:
  const PortalName name_;
  const PortalAddress peer_address_;
  const Side side_;
  const os::Memory::Mapping control_block_mapping_;
  PortalControlBlock& control_block_{
      *control_block_mapping_.As<PortalControlBlock>()};
  PortalControlBlock::SideState& my_shared_state_{control_block_.sides[side_]};
  PortalControlBlock::SideState& their_shared_state{
      control_block_.sides[Opposite(side_)]};

  absl::Mutex mutex_;
  bool closed_ ABSL_GUARDED_BY(mutex_) = false;
  absl::optional<Parcel> pending_parcel_ ABSL_GUARDED_BY(mutex_);
  ParcelQueue outgoing_parcels_ ABSL_GUARDED_BY(mutex_);
  ParcelQueue incoming_parcels_ ABSL_GUARDED_BY(mutex_);
  bool in_two_phase_get_ ABSL_GUARDED_BY(mutex_) = false;
  absl::flat_hash_set<std::unique_ptr<Trap>> traps_ ABSL_GUARDED_BY(mutex_);
  IpczPortalStatus status_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTED_PORTAL_BACKEND_H_
