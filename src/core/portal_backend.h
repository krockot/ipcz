// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_BACKEND_H_
#define IPCZ_SRC_CORE_PORTAL_BACKEND_H_

#include <cstdint>
#include <utility>

#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class Node;
class Portal;

// Base class for an implementation backing a Portal. A Portal may switch from
// one backend to another if its peer is moved onto or off of the same node.
// Compare DirectPortalBackend with RoutedPortalBackend.
class PortalBackend {
 public:
  PortalBackend();
  virtual ~PortalBackend();

  // Back-reference to our owning Portal. This is managed by the portal itself,
  // is only modified by one portal at a time and only under the portal's own
  // internal mutex. Null if the backend is in transit.
  void set_portal(Portal* portal) { portal_ = portal; }
  Portal* portal() const { return portal_; }

  virtual IpczResult Close() = 0;
  virtual IpczResult QueryStatus(IpczPortalStatusFieldFlags field_flags,
                                 IpczPortalStatus& status) = 0;

  virtual IpczResult Put(absl::Span<const uint8_t> data,
                         absl::Span<const IpczHandle> portals,
                         absl::Span<const IpczOSHandle> os_handles,
                         const IpczPutLimits* limits) = 0;
  virtual IpczResult BeginPut(uint32_t& num_data_bytes,
                              IpczBeginPutFlags flags,
                              const IpczPutLimits* limits,
                              void** data) = 0;
  virtual IpczResult CommitPut(uint32_t num_data_bytes_produced,
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
                              IpczHandle* portals,
                              uint32_t* num_portals,
                              IpczOSHandle* os_handles,
                              uint32_t* num_os_handles) = 0;
  virtual IpczResult CommitGet(uint32_t num_data_bytes_consumed) = 0;
  virtual IpczResult AbortGet() = 0;

  virtual IpczResult CreateMonitor(const IpczMonitorDescriptor& descriptor,
                                   IpczHandle* handle) = 0;

 private:
  Portal* portal_ = nullptr;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_BACKEND_H_
