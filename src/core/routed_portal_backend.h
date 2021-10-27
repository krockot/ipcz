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
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {
namespace core {

class BufferingPortalBackend;
class Node;

// PortalBackend implementation for a portal whose peer may live in a different
// node.
class RoutedPortalBackend : public PortalBackend {
 public:
  RoutedPortalBackend(const PortalName& name,
                      const PortalAddress& peer_address);
  ~RoutedPortalBackend() override;

  void AdoptBufferingBackendState(BufferingPortalBackend& backend);

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
  const PortalName name_;

  struct Parcel {
    Parcel();
    Parcel(Parcel&& other);
    Parcel& operator=(Parcel&& other);
    ~Parcel();

    std::vector<uint8_t> data;
    std::vector<mem::Ref<Portal>> portals;
    std::vector<os::Handle> os_handles;

    // Offset into data from which the next bytes will be read. Non-zero only
    // for a parcel which has already been partially consumed by a get
    // operation.
    size_t data_offset = 0;

    // The next parcel in queue.
    std::unique_ptr<Parcel> next_parcel;
  };

  std::vector<uint8_t> ConsumeParcel(IpczHandle* portals,
                                     IpczOSHandle* os_handles);
  void ConsumePartialParcel(size_t num_bytes_consumed,
                            IpczHandle* portals,
                            IpczOSHandle* os_handles);
  void ConsumePortalsAndHandles(Parcel& parcel,
                                IpczHandle* portals,
                                IpczOSHandle* os_handles);
  absl::Mutex mutex_;
  PortalAddress peer_address_ ABSL_GUARDED_BY(mutex_);
  bool closed_ ABSL_GUARDED_BY(mutex_) = false;
  bool peer_closed_ ABSL_GUARDED_BY(mutex_) = false;
  std::unique_ptr<Parcel> pending_parcel_ ABSL_GUARDED_BY(mutex_);
  std::unique_ptr<Parcel> next_outgoing_parcel_ ABSL_GUARDED_BY(mutex_);
  Parcel* last_outgoing_parcel_ ABSL_GUARDED_BY(mutex_) = nullptr;
  std::unique_ptr<Parcel> next_incoming_parcel_ ABSL_GUARDED_BY(mutex_);
  Parcel* last_incoming_parcel_ ABSL_GUARDED_BY(mutex_) = nullptr;
  size_t num_incoming_parcels_ ABSL_GUARDED_BY(mutex_) = 0;
  size_t num_incoming_bytes_ ABSL_GUARDED_BY(mutex_) = 0;
  size_t num_outgoing_parcels_ ABSL_GUARDED_BY(mutex_) = 0;
  size_t num_outgoing_bytes_ ABSL_GUARDED_BY(mutex_) = 0;
  bool in_two_phase_get_ ABSL_GUARDED_BY(mutex_) = false;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTED_PORTAL_BACKEND_H_
