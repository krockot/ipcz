// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_H_
#define IPCZ_SRC_CORE_PORTAL_H_

#include <cstdint>
#include <list>
#include <utility>
#include <vector>

#include "core/portal_backend_observer.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_set.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class Node;
class PortalBackend;

class Portal : private PortalBackendObserver {
 public:
  using Pair = std::pair<std::unique_ptr<Portal>, std::unique_ptr<Portal>>;

  explicit Portal(std::unique_ptr<PortalBackend> backend);
  ~Portal() override;

  static Pair CreateLocalPair(mem::Ref<Node> node);

  static std::unique_ptr<Portal> CreateRouted(mem::Ref<Node> node);

  std::unique_ptr<PortalBackend> TakeBackend();
  void SetBackend(std::unique_ptr<PortalBackend> backend);

  IpczResult Close();
  IpczResult QueryStatus(IpczPortalStatusFieldFlags field_flags,
                         IpczPortalStatus& status);

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
                        IpczPortalStatusFieldFlags status_fields,
                        IpczHandle* trap);
  IpczResult ArmTrap(IpczHandle trap,
                     IpczTrapConditions* satisfied_conditions,
                     IpczPortalStatus* status);
  IpczResult DestroyTrap(IpczHandle trap);

 private:
  struct Trap {
    IpczTrapConditions conditions;
    IpczTrapEventHandler handler;
    uintptr_t context;
    IpczPortalStatusFieldFlags status_fields;
  };

  // PortalBackendObserver:
  void OnPeerClosed(const PortalBackendStatus& status) override;
  void OnPortalDead(const PortalBackendStatus& status) override;
  void OnQueueChanged(const PortalBackendStatus& status) override;
  void OnPeerQueueChanged(const PortalBackendStatus& status) override;

  absl::Mutex mutex_;
  std::unique_ptr<PortalBackend> backend_ ABSL_GUARDED_BY(mutex_);
  absl::flat_hash_set<std::unique_ptr<Trap>> traps_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_H_
