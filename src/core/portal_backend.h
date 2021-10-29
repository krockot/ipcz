// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_BACKEND_H_
#define IPCZ_SRC_CORE_PORTAL_BACKEND_H_

#include <cstdint>
#include <utility>
#include <vector>

#include "core/node.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class Parcel;
class Portal;
class Router;
class Trap;

// Base class for an implementation backing a Portal. A Portal may switch from
// one backend to another if its peer is moved onto or off of the same node.
class PortalBackend {
 public:
  enum class Type {
    kDirect,
    kBuffering,
    kRouted,
  };

  virtual ~PortalBackend() {}

  virtual Type GetType() const = 0;
  virtual bool CanTravelThroughPortal(Portal& sender) = 0;
  virtual bool AcceptParcel(Parcel& parcel) = 0;
  virtual IpczResult Close(
      Node::LockedRouter& router,
      std::vector<mem::Ref<Portal>>& other_portals_to_close) = 0;
  virtual IpczResult QueryStatus(IpczPortalStatus& status) = 0;
  virtual IpczResult Put(Node::LockedRouter& router,
                         absl::Span<const uint8_t> data,
                         absl::Span<const IpczHandle> portals,
                         absl::Span<const IpczOSHandle> os_handles,
                         const IpczPutLimits* limits) = 0;
  virtual IpczResult BeginPut(IpczBeginPutFlags flags,
                              const IpczPutLimits* limits,
                              uint32_t& num_data_bytes,
                              void** data) = 0;
  virtual IpczResult CommitPut(Node::LockedRouter& router,
                               uint32_t num_data_bytes_produced,
                               absl::Span<const IpczHandle> portals,
                               absl::Span<const IpczOSHandle> os_handles) = 0;
  virtual IpczResult AbortPut() = 0;
  virtual IpczResult Get(void* data,
                         uint32_t* num_data_bytes,
                         IpczHandle* portals,
                         uint32_t* num_portals,
                         IpczOSHandle* os_handles,
                         uint32_t* num_os_handles) = 0;
  virtual IpczResult BeginGet(const void** data,
                              uint32_t* num_data_bytes,
                              uint32_t* num_portals,
                              uint32_t* num_os_handles) = 0;
  virtual IpczResult CommitGet(uint32_t num_data_bytes_consumed,
                               IpczHandle* portals,
                               uint32_t* num_portals,
                               IpczOSHandle* os_handles,
                               uint32_t* num_os_handles) = 0;
  virtual IpczResult AbortGet() = 0;
  virtual IpczResult AddTrap(std::unique_ptr<Trap> trap) = 0;
  virtual IpczResult ArmTrap(Trap& trap,
                             IpczTrapConditions* satisfied_conditions,
                             IpczPortalStatus* status) = 0;
  virtual IpczResult RemoveTrap(Trap& trap) = 0;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_BACKEND_H_
