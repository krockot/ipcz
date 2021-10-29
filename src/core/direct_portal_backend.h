// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_DIRECT_PORTAL_BACKEND_H_
#define IPCZ_SRC_CORE_DIRECT_PORTAL_BACKEND_H_

#include <cstdint>
#include <utility>

#include "core/portal_backend.h"
#include "core/side.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"

namespace ipcz {
namespace core {

class Portal;

// PortalBackend implementation for a portal whose peer lives in the same node.
// This backend grants portals direct access to each others' state for more
// efficient operations with no dependency on Node state or routing behavior.
class DirectPortalBackend : public PortalBackend {
 public:
  using Pair = std::pair<std::unique_ptr<DirectPortalBackend>,
                         std::unique_ptr<DirectPortalBackend>>;

  ~DirectPortalBackend() override;

  static Pair CreatePair(Portal& portal0, Portal& portal1);

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
  struct SharedState;
  struct PortalState;

  DirectPortalBackend(mem::Ref<SharedState> state, Side side);

  PortalState& this_side();
  PortalState& other_side();

  const mem::Ref<SharedState> state_;
  const Side side_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_DIRECT_PORTAL_BACKEND_H_
