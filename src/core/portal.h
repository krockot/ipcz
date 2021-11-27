// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_H_
#define IPCZ_SRC_CORE_PORTAL_H_

#include <cstdint>
#include <utility>

#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class Node;
class Router;

class Portal : public mem::RefCounted {
 public:
  Portal(mem::Ref<Node> node, mem::Ref<Router> router);

  const mem::Ref<Node>& node() const { return node_; }
  const mem::Ref<Router>& router() const { return router_; }

  static std::pair<mem::Ref<Portal>, mem::Ref<Portal>> CreatePair(
      mem::Ref<Node> node);

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
  ~Portal() override;

  const mem::Ref<Node> node_;
  const mem::Ref<Router> router_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_H_
