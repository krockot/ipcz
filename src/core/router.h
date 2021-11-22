// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTER_H_
#define IPCZ_SRC_CORE_ROUTER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/incoming_parcel_queue.h"
#include "core/outgoing_parcel_queue.h"
#include "core/parcel.h"
#include "core/routing_id.h"
#include "core/routing_mode.h"
#include "core/sequence_number.h"
#include "core/side.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class NodeLink;
struct PortalDescriptor;
class RouterLink;
class RouterObserver;

class Router : public mem::RefCounted {
 public:
  explicit Router(Side side);

  void SetObserver(mem::Ref<RouterObserver> observer);
  mem::Ref<RouterObserver> GetObserver();

  // Returns true iff this Router's peer link is a LocalRouterLink and its local
  // peer is `other`.
  bool HasLocalPeer(const mem::Ref<Router>& router);

  // Returns true iff sending a parcel of `data_size` towards the other side of
  // the route may exceed the specified `limits` on the receiving end.
  bool WouldOutgoingParcelExceedLimits(size_t data_size,
                                       const IpczPutLimits& limits);

  // Returns true iff accepting an incoming parcel of `data_size` would cause
  // this router's incoming parcel queue to exceed limits specified by `limits`.
  bool WouldIncomingParcelExceedLimits(size_t data_size,
                                       const IpczPutLimits& limits);

  // Attempts to send an outgoing parcel originating from this Router. Called
  // only as a direct result of a Put() call on the router's owning portal.
  IpczResult SendOutgoingParcel(absl::Span<const uint8_t> data,
                                Parcel::PortalVector& portals,
                                std::vector<os::Handle>& os_handles);

  // Closes this side of the Router's own route. Only called on a Router to
  // which a Portal is currently attached, and only by that Portal.
  void CloseRoute();

  // Activates a buffering router, using `link` as its new peer link.
  void ActivateWithPeer(mem::Ref<RouterLink> link);

  // Activates a buffering router, using `link` as its new predecessor link.
  void ActivateWithPredecessor(mem::Ref<RouterLink> link);

  // Provides the Router with a new successor link to which it should forward
  // all incoming parcels. If the Router is in full proxying mode, it may also
  // listen for outgoing parcels from the same link, to be forwarded to its peer
  // or predecessor.
  void BeginProxyingWithSuccessor(mem::Ref<RouterLink> link);

  // A special case used when splitting a local pair over a new router link.
  // Similar to BeginProxyingWithSuccessor, this portal treats `link` as its
  // successor link; but rather than coordinating a half-proxy bypass, the
  // our former local peer is also updated atomically to use `link` as its new
  // peer link.
  void BeginProxyingWithSuccessorAndUpdateLocalPeer(mem::Ref<RouterLink> link);

  // Accepts a parcel routed here from `link` via `routing_id`, which is
  // determined to be either an incoming or outgoing parcel based on the source
  // and current routing mode.
  bool AcceptParcelFrom(NodeLink& link, RoutingId routing_id, Parcel& parcel);

  // Accepts an incoming parcel routed here from some other Router. What happens
  // to the parcel depends on the Router's current RoutingMode and established
  // links to other Routers.
  bool AcceptIncomingParcel(Parcel& parcel);

  // Accepts an outgoing parcel router here from some other Router. What happens
  // to the parcel depends on the Router's current RoutingMode and its
  // established links to other Routers.
  bool AcceptOutgoingParcel(Parcel& parcel);

  // Accepts notification that one `side` of this route has been closed.
  // Depending on current routing mode and established links, this notification
  // may be propagated elsewhere by this Router.
  void AcceptRouteClosure(Side side, SequenceNumber sequence_length);

  // Retrieves the next available incoming parcel from this Router, if present.
  IpczResult GetNextIncomingParcel(void* data,
                                   uint32_t* num_bytes,
                                   IpczHandle* portals,
                                   uint32_t* num_portals,
                                   IpczOSHandle* os_handles,
                                   uint32_t* num_os_handles);

  mem::Ref<Router> Serialize(PortalDescriptor& descriptor);

 private:
  friend class LocalRouterLink;

  ~Router() override;

  void FlushProxiedParcels();

  const Side side_;

  absl::Mutex mutex_;
  SequenceNumber outgoing_sequence_length_ = 0;
  mem::Ref<RouterObserver> observer_ ABSL_GUARDED_BY(mutex_);
  RoutingMode routing_mode_ ABSL_GUARDED_BY(mutex_) = RoutingMode::kBuffering;
  mem::Ref<RouterLink> peer_ ABSL_GUARDED_BY(mutex_);
  mem::Ref<RouterLink> successor_ ABSL_GUARDED_BY(mutex_);
  mem::Ref<RouterLink> predecessor_ ABSL_GUARDED_BY(mutex_);
  OutgoingParcelQueue buffered_parcels_ ABSL_GUARDED_BY(mutex_);
  IncomingParcelQueue incoming_parcels_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTER_H_
