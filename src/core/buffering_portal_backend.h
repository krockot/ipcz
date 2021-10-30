// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_BUFFERING_PORTAL_BACKEND_H_
#define IPCZ_SRC_CORE_BUFFERING_PORTAL_BACKEND_H_

#include <cstddef>
#include <cstdint>
#include <forward_list>
#include <utility>
#include <vector>

#include "core/parcel.h"
#include "core/parcel_queue.h"
#include "core/portal_backend.h"
#include "core/side.h"
#include "core/trap.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace ipcz {
namespace core {

class Portal;

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
  bool AcceptParcel(Parcel& parcel, TrapEventDispatcher& dispatcher) override;
  bool NotifyPeerClosed(TrapEventDispatcher& dispatcher) override;
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
  friend class RoutedPortalBackend;

  const Side side_;

  absl::Mutex mutex_;
  bool closed_ ABSL_GUARDED_BY(mutex_) = false;
  IpczPortalStatus status_ ABSL_GUARDED_BY(mutex_);
  absl::optional<Parcel> pending_parcel_ ABSL_GUARDED_BY(mutex_);
  ParcelQueue outgoing_parcels_ ABSL_GUARDED_BY(mutex_);
  TrapSet traps_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_BUFFERING_PORTAL_BACKEND_H_
