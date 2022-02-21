// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_PORTAL_H_
#define IPCZ_SRC_IPCZ_PORTAL_H_

#include <cstdint>
#include <utility>

#include "ipcz/api_object.h"
#include "ipcz/ipcz.h"
#include "ipcz/parcel.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/ref_counted.h"

namespace ipcz {

class Node;
class Router;

// A Portal owns a terminal Router along a route. Portals are manipulated
// directly by public ipcz API calls.
class Portal : public APIObject {
 public:
  using Pair = std::pair<Ref<Portal>, Ref<Portal>>;

  Portal(Ref<Node> node, Ref<Router> router);

  static constexpr ObjectType object_type() { return kPortal; }

  const Ref<Node>& node() const { return node_; }
  const Ref<Router>& router() const { return router_; }

  static Pair CreatePair(Ref<Node> node);

  // APIObject:
  IpczResult Close() override;
  bool CanSendFrom(Portal& sender) override;

  // ipcz portal API implementation:
  IpczResult Merge(Portal& other);
  IpczResult QueryStatus(IpczPortalStatus& status);

  IpczResult Put(absl::Span<const uint8_t> data,
                 absl::Span<const IpczHandle> handles,
                 absl::Span<const IpczOSHandle> ipcz_os_handles,
                 const IpczPutLimits* limits);
  IpczResult BeginPut(IpczBeginPutFlags flags,
                      const IpczPutLimits* limits,
                      uint32_t& num_data_bytes,
                      void** data);
  IpczResult CommitPut(uint32_t num_data_bytes_produced,
                       absl::Span<const IpczHandle> handles,
                       absl::Span<const IpczOSHandle> ipcz_os_handles);
  IpczResult AbortPut();

  IpczResult Get(void* data,
                 uint32_t* num_data_bytes,
                 IpczHandle* handles,
                 uint32_t* num_handles,
                 IpczOSHandle* os_handles,
                 uint32_t* num_os_handles);
  IpczResult BeginGet(const void** data,
                      uint32_t* num_data_bytes,
                      uint32_t* num_handles,
                      uint32_t* num_os_handles);
  IpczResult CommitGet(uint32_t num_data_bytes_consumed,
                       IpczHandle* handles,
                       uint32_t* num_handles,
                       IpczOSHandle* os_handles,
                       uint32_t* num_os_handles);
  IpczResult AbortGet();

  IpczResult CreateTrap(const IpczTrapConditions& conditions,
                        IpczTrapEventHandler handler,
                        uint64_t context,
                        IpczHandle& trap);
  IpczResult ArmTrap(IpczHandle trap,
                     IpczTrapConditionFlags* satisfied_condition_flags,
                     IpczPortalStatus* status);
  IpczResult DestroyTrap(IpczHandle trap);

 private:
  ~Portal() override;

  const Ref<Node> node_;
  const Ref<Router> router_;

  absl::Mutex mutex_;
  absl::optional<Parcel> pending_parcel_ ABSL_GUARDED_BY(mutex_);
  bool in_two_phase_put_ ABSL_GUARDED_BY(mutex_) = false;
  bool in_two_phase_get_ ABSL_GUARDED_BY(mutex_) = false;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_PORTAL_H_
