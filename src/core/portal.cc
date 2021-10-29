// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/portal.h"

#include <memory>
#include <utility>

#include "core/buffering_portal_backend.h"
#include "core/direct_portal_backend.h"
#include "core/name.h"
#include "core/node.h"
#include "core/portal_backend.h"
#include "core/portal_control_block.h"
#include "core/routed_portal_backend.h"
#include "core/trap.h"
#include "debug/log.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/handle_util.h"

namespace ipcz {
namespace core {

namespace {

bool ValidatePortalsForTravel(Portal& sender,
                              absl::Span<const IpczHandle> handles) {
  for (IpczHandle handle : handles) {
    if (!ToRef<Portal>(handle).CanTravelThroughPortal(sender)) {
      return false;
    }
  }
  return true;
}

}  // namespace

Portal::Portal(Node& node) : Portal(node, nullptr, /*transferrable=*/true) {}

Portal::Portal(Node& node, std::unique_ptr<PortalBackend> backend)
    : Portal(node, std::move(backend), /*transferrable=*/true) {}

Portal::Portal(Node& node,
               std::unique_ptr<PortalBackend> backend,
               decltype(kNonTransferrable))
    : Portal(node, std::move(backend), /*transferrable=*/false) {}

Portal::Portal(Node& node,
               std::unique_ptr<PortalBackend> backend,
               bool transferrable)
    : node_(mem::WrapRefCounted(&node)),
      transferrable_(transferrable),
      backend_(std::move(backend)) {}

Portal::~Portal() = default;

// static
Portal::Pair Portal::CreateLocalPair(Node& node) {
  auto portal0 = mem::MakeRefCounted<Portal>(node);
  auto portal1 = mem::MakeRefCounted<Portal>(node);
  DirectPortalBackend::Pair backends =
      DirectPortalBackend::CreatePair(*portal0, *portal1);
  portal0->SetBackend(std::move(backends.first));
  portal1->SetBackend(std::move(backends.second));
  return {std::move(portal0), std::move(portal1)};
}

std::unique_ptr<PortalBackend> Portal::TakeBackend() {
  absl::MutexLock lock(&mutex_);
  return std::move(backend_);
}

void Portal::SetBackend(std::unique_ptr<PortalBackend> backend) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!backend_);
  backend_ = std::move(backend);
}

bool Portal::CanTravelThroughPortal(Portal& sender) {
  if (!transferrable_) {
    return false;
  }

  absl::MutexLock lock(&mutex_);
  return &sender != this && backend_->CanTravelThroughPortal(sender);
}

bool Portal::StartRouting(Node::LockedRouter& router,
                          const PortalName& my_name,
                          const PortalAddress& peer_address,
                          os::Memory::Mapping control_block_mapping) {
  absl::MutexLock lock(&mutex_);
  if (!backend_ || backend_->GetType() != PortalBackend::Type::kBuffering) {
    return false;
  }

  auto& backend = reinterpret_cast<BufferingPortalBackend&>(*backend_);
  auto new_backend = std::make_unique<RoutedPortalBackend>(
      my_name, peer_address, backend.side(), std::move(control_block_mapping));
  new_backend->AdoptBufferingBackendState(router, backend);
  backend_ = std::move(new_backend);
  return true;
}

bool Portal::AcceptParcel(Parcel& parcel, TrapEventDispatcher& dispatcher) {
  absl::MutexLock lock(&mutex_);
  return backend_->AcceptParcel(parcel, dispatcher);
}

IpczResult Portal::Close() {
  std::vector<mem::Ref<Portal>> other_portals_to_close;
  {
    Node::LockedRouter router(*node_);
    absl::MutexLock lock(&mutex_);
    IpczResult result = backend_->Close(router, other_portals_to_close);
    if (result != IPCZ_RESULT_OK) {
      return result;
    }
  }

  for (mem::Ref<Portal>& portal : other_portals_to_close) {
    portal->Close();
  }
  return IPCZ_RESULT_OK;
}

IpczResult Portal::QueryStatus(IpczPortalStatus& status) {
  absl::MutexLock lock(&mutex_);
  return backend_->QueryStatus(status);
}

IpczResult Portal::Put(absl::Span<const uint8_t> data,
                       absl::Span<const IpczHandle> portals,
                       absl::Span<const IpczOSHandle> os_handles,
                       const IpczPutLimits* limits) {
  if (!ValidatePortalsForTravel(*this, portals)) {
    // At least one of the portals given was either `this` or its peer, which is
    // not allowed.
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  Node::LockedRouter router(*node_);
  absl::MutexLock lock(&mutex_);
  return backend_->Put(router, data, portals, os_handles, limits);
}

IpczResult Portal::BeginPut(IpczBeginPutFlags flags,
                            const IpczPutLimits* limits,
                            uint32_t& num_data_bytes,
                            void** data) {
  absl::MutexLock lock(&mutex_);
  return backend_->BeginPut(flags, limits, num_data_bytes, data);
}

IpczResult Portal::CommitPut(uint32_t num_data_bytes_produced,
                             absl::Span<const IpczHandle> portals,
                             absl::Span<const IpczOSHandle> os_handles) {
  if (!ValidatePortalsForTravel(*this, portals)) {
    // At least one of the portals given was either `this` or its peer, which is
    // not allowed.
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  Node::LockedRouter router(*node_);
  absl::MutexLock lock(&mutex_);
  return backend_->CommitPut(router, num_data_bytes_produced, portals,
                             os_handles);
}

IpczResult Portal::AbortPut() {
  absl::MutexLock lock(&mutex_);
  return backend_->AbortPut();
}

IpczResult Portal::Get(void* data,
                       uint32_t* num_data_bytes,
                       IpczHandle* portals,
                       uint32_t* num_portals,
                       IpczOSHandle* os_handles,
                       uint32_t* num_os_handles) {
  absl::MutexLock lock(&mutex_);
  return backend_->Get(data, num_data_bytes, portals, num_portals, os_handles,
                       num_os_handles);
}

IpczResult Portal::BeginGet(const void** data,
                            uint32_t* num_data_bytes,
                            uint32_t* num_portals,
                            uint32_t* num_os_handles) {
  absl::MutexLock lock(&mutex_);
  return backend_->BeginGet(data, num_data_bytes, num_portals, num_os_handles);
}

IpczResult Portal::CommitGet(uint32_t num_data_bytes_consumed,
                             IpczHandle* portals,
                             uint32_t* num_portals,
                             IpczOSHandle* os_handles,
                             uint32_t* num_os_handles) {
  absl::MutexLock lock(&mutex_);
  return backend_->CommitGet(num_data_bytes_consumed, portals, num_portals,
                             os_handles, num_os_handles);
}

IpczResult Portal::AbortGet() {
  absl::MutexLock lock(&mutex_);
  return backend_->AbortGet();
}

IpczResult Portal::CreateTrap(const IpczTrapConditions& conditions,
                              IpczTrapEventHandler handler,
                              uintptr_t context,
                              IpczHandle& trap) {
  auto new_trap = std::make_unique<Trap>(conditions, handler, context);
  trap = ToHandle(new_trap.get());

  absl::MutexLock lock(&mutex_);
  return backend_->AddTrap(std::move(new_trap));
}

IpczResult Portal::ArmTrap(IpczHandle trap,
                           IpczTrapConditions* satisfied_conditions,
                           IpczPortalStatus* status) {
  absl::MutexLock lock(&mutex_);
  return backend_->ArmTrap(ToRef<Trap>(trap), satisfied_conditions, status);
}

IpczResult Portal::DestroyTrap(IpczHandle trap) {
  absl::MutexLock lock(&mutex_);
  return backend_->RemoveTrap(ToRef<Trap>(trap));
}

}  // namespace core
}  // namespace ipcz
