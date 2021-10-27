// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_BUFFERING_PORTAL_BACKEND_H_
#define IPCZ_SRC_CORE_BUFFERING_PORTAL_BACKEND_H_

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "core/portal_backend.h"
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
  BufferingPortalBackend();
  ~BufferingPortalBackend() override;

  // PortalBackend:
  Type GetType() const override;
  bool CanTravelThroughPortal(Portal& sender) override;
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
                     IpczTrapConditions* satisfied_conditions,
                     IpczPortalStatus* status) override;
  IpczResult RemoveTrap(Trap& trap) override;

 private:
  friend class RoutedPortalBackend;

  struct Parcel {
    Parcel();
    Parcel(Parcel&& other);
    Parcel& operator=(Parcel&& other);
    ~Parcel();
    std::vector<uint8_t> data;
    std::vector<mem::Ref<Portal>> portals;
    std::vector<os::Handle> os_handles;
  };

  absl::Mutex mutex_;
  bool closed_ ABSL_GUARDED_BY(mutex_) = false;
  absl::optional<Parcel> pending_parcel_ ABSL_GUARDED_BY(mutex_);
  std::vector<Parcel> outgoing_parcels_ ABSL_GUARDED_BY(mutex_);
  size_t num_outgoing_bytes_ ABSL_GUARDED_BY(mutex_) = 0;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_BUFFERING_PORTAL_BACKEND_H_
