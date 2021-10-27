// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/buffering_portal_backend.h"

#include <utility>

#include "core/node.h"
#include "core/trap.h"

namespace ipcz {
namespace core {

BufferingPortalBackend::BufferingPortalBackend() = default;

BufferingPortalBackend::~BufferingPortalBackend() = default;

PortalBackend::Type BufferingPortalBackend::GetType() const {
  return Type::kBuffering;
}

bool BufferingPortalBackend::CanTravelThroughPortal(Portal& sender) {
  return true;
}

IpczResult BufferingPortalBackend::Close(
    Node::LockedRouter& router,
    std::vector<mem::Ref<Portal>>& other_portals_to_close) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult BufferingPortalBackend::QueryStatus(IpczPortalStatus& status) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult BufferingPortalBackend::Put(
    Node::LockedRouter& router,
    absl::Span<const uint8_t> data,
    absl::Span<const IpczHandle> portals,
    absl::Span<const IpczOSHandle> os_handles,
    const IpczPutLimits* limits) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult BufferingPortalBackend::BeginPut(IpczBeginPutFlags flags,
                                            const IpczPutLimits* limits,
                                            uint32_t& num_data_bytes,
                                            void** data) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult BufferingPortalBackend::CommitPut(
    Node::LockedRouter& router,
    uint32_t num_data_bytes_produced,
    absl::Span<const IpczHandle> portals,
    absl::Span<const IpczOSHandle> os_handles) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult BufferingPortalBackend::AbortPut() {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult BufferingPortalBackend::Get(void* data,
                                       uint32_t* num_data_bytes,
                                       IpczHandle* portals,
                                       uint32_t* num_portals,
                                       IpczOSHandle* os_handles,
                                       uint32_t* num_os_handles) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult BufferingPortalBackend::BeginGet(const void** data,
                                            uint32_t* num_data_bytes,
                                            uint32_t* num_portals,
                                            uint32_t* num_os_handles) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult BufferingPortalBackend::CommitGet(uint32_t num_data_bytes_consumed,
                                             IpczHandle* portals,
                                             uint32_t* num_portals,
                                             IpczOSHandle* os_handles,
                                             uint32_t* num_os_handles) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult BufferingPortalBackend::AbortGet() {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult BufferingPortalBackend::AddTrap(std::unique_ptr<Trap> trap) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult BufferingPortalBackend::ArmTrap(
    Trap& trap,
    IpczTrapConditions* satisfied_conditions,
    IpczPortalStatus* status) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult BufferingPortalBackend::RemoveTrap(Trap& trap) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

}  // namespace core
}  // namespace ipcz
