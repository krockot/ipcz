// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTED_PORTAL_BACKEND_H_
#define IPCZ_SRC_CORE_ROUTED_PORTAL_BACKEND_H_

#include <cstdint>
#include <utility>

#include "core/name.h"
#include "core/portal_backend.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"

namespace ipcz {
namespace core {

class Node;

// PortalBackend implementation for a portal whose peer may live in a different
// node.
class RoutedPortalBackend : public PortalBackend {
 public:
  RoutedPortalBackend(mem::Ref<Node> node, const PortalName& name);
  ~RoutedPortalBackend() override;

  // PortalBackend:
  IpczResult Close() override;
  IpczResult QueryStatus(IpczPortalStatus& status) override;
  IpczResult Put(absl::Span<const uint8_t> data,
                 absl::Span<const IpczHandle> portals,
                 absl::Span<const IpczOSHandle> os_handles,
                 const IpczPutLimits* limits) override;
  IpczResult BeginPut(IpczBeginPutFlags flags,
                      const IpczPutLimits* limits,
                      uint32_t& num_data_bytes,
                      void** data) override;
  IpczResult CommitPut(uint32_t num_data_bytes_produced,
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
  const mem::Ref<Node> node_;
  const PortalName name_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTED_PORTAL_BACKEND_H_
