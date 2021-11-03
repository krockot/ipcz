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
#include "core/portal_backend.h"
#include "core/portal_backend_state.h"
#include "core/portal_control_block.h"
#include "core/side.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {
namespace core {

class BufferingPortalBackend;
class NodeLink;

// PortalBackend implementation for a portal whose peer may live in a different
// node.
class RoutedPortalBackend : public PortalBackend {
 public:
  RoutedPortalBackend(const PortalName& name,
                      mem::Ref<NodeLink> link,
                      const PortalName& remote_portal,
                      Side side,
                      os::Memory::Mapping control_block_mapping);
  ~RoutedPortalBackend() override;

  bool AdoptBufferingBackendState(BufferingPortalBackend& backend);
  std::unique_ptr<BufferingPortalBackend> StartBuffering();

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
  const PortalName name_;
  const mem::Ref<NodeLink> link_;
  const PortalName remote_portal_;
  const Side side_;
  const os::Memory::Mapping control_block_mapping_;
  PortalControlBlock& control_block_{
      *control_block_mapping_.As<PortalControlBlock>()};

  absl::Mutex mutex_;
  PortalBackendState state_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTED_PORTAL_BACKEND_H_
