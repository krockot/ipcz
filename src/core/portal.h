// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_H_
#define IPCZ_SRC_CORE_PORTAL_H_

#include <cstdint>
#include <utility>

#include "core/name.h"
#include "core/node.h"
#include "core/side.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class Parcel;
class PortalBackend;
class TrapEventDispatcher;

class Portal : public mem::RefCounted {
 public:
  enum { kNonTransferrable };

  using Pair = std::pair<mem::Ref<Portal>, mem::Ref<Portal>>;

  explicit Portal(Node& node);
  Portal(Node& node, std::unique_ptr<PortalBackend> backend);
  Portal(Node& node,
         std::unique_ptr<PortalBackend> backend,
         decltype(kNonTransferrable));

  static Pair CreateLocalPair(Node& node);

  std::unique_ptr<PortalBackend> TakeBackend();
  void SetBackend(std::unique_ptr<PortalBackend> backend);

  bool CanTravelThroughPortal(Portal& sender);

  // Transitions from buffering to routing.
  bool StartRouting(Node::LockedRouter& router,
                    const PortalName& my_name,
                    const PortalAddress& peer_address,
                    os::Memory::Mapping control_block_mapping);

  // Accepts a parcel from an external source, e.g. as routed from another node.
  bool AcceptParcel(Parcel& parcel, TrapEventDispatcher& dispatcher);

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
                     IpczTrapConditions* satisfied_conditions,
                     IpczPortalStatus* status);
  IpczResult DestroyTrap(IpczHandle trap);

 private:
  Portal(Node& node,
         std::unique_ptr<PortalBackend> backend,
         bool transferrable);
  ~Portal() override;

  const mem::Ref<Node> node_;
  const bool transferrable_;

  absl::Mutex mutex_;
  std::unique_ptr<PortalBackend> backend_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_H_
