// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_BUFFERING_PORTAL_BACKEND_H_
#define IPCZ_SRC_CORE_BUFFERING_PORTAL_BACKEND_H_

#include "core/name.h"
#include "core/portal_backend.h"
#include "core/portal_backend_state.h"
#include "core/side.h"
#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace ipcz {
namespace core {

// PortalBackend implementation for a portal whose remote peer is either unknown
// or temporarily unreachable. A buffering backend never receives parcels, and
// it queues all parcels put into it locally. When a Node has enough information
// to begin transmitting parcels to and from the owning portal, this is replaced
// with a RoutedPortalBackend.
class BufferingPortalBackend : public PortalBackend {
 public:
  explicit BufferingPortalBackend(Side side);
  ~BufferingPortalBackend() override;

  Side side() const { return side_; }

  // PortalBackend:
  Type GetType() const override;
  bool CanTravelThroughPortal(Portal& sender) override;
  void PrepareForTravel(PortalInTransit& portal_in_transit) override;
  bool AcceptParcel(Parcel& parcel, TrapEventDispatcher& dispatcher) override;
  bool NotifyPeerClosed(TrapEventDispatcher& dispatcher) override;
  IpczResult Close(
      std::vector<mem::Ref<Portal>>& other_portals_to_close) override;
  IpczResult QueryStatus(IpczPortalStatus& status) override;
  IpczResult Put(absl::Span<const uint8_t> data,
                 absl::Span<PortalInTransit> portals,
                 absl::Span<const IpczOSHandle> os_handles,
                 const IpczPutLimits* limits) override;
  IpczResult BeginPut(IpczBeginPutFlags flags,
                      const IpczPutLimits* limits,
                      uint32_t& num_data_bytes,
                      void** data) override;
  IpczResult CommitPut(uint32_t num_data_bytes_produced,
                       absl::Span<PortalInTransit> portals,
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
  friend class DirectPortalBackend;
  friend class NodeLink;
  friend class RoutedPortalBackend;

  const Side side_;

  absl::Mutex mutex_;
  absl::optional<PortalName> routed_name_;
  PortalBackendState state_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_BUFFERING_PORTAL_BACKEND_H_
