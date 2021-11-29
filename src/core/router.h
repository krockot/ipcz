// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTER_H_
#define IPCZ_SRC_CORE_ROUTER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/incoming_parcel_queue.h"
#include "core/node_name.h"
#include "core/parcel.h"
#include "core/routing_id.h"
#include "core/sequence_number.h"
#include "core/side.h"
#include "core/trap.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/numeric/int128.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class NodeLink;
struct PortalDescriptor;
class RouterLink;

class Router : public mem::RefCounted {
 public:
  explicit Router(Side side);

  // Pauses or unpauses outward parcel transmission.
  void PauseOutboundTransmission(bool paused);

  // Returns true iff the other side of this Router's route is known to be
  // closed.
  bool IsPeerClosed();

  // Returns true iff the other side of this Router's route is known to be
  // closed, AND all parcels sent from that side have already been retrieved by
  // the application on this side.
  bool IsRouteDead();

  // Fills in an IpczPortalStatus corresponding to the current state of this
  // Router.
  void QueryStatus(IpczPortalStatus& status);

  // Returns true iff this Router's outward link is a LocalRouterLink between
  // `this` and `router`.
  bool HasLocalPeer(const mem::Ref<Router>& router);

  // Returns true iff sending a parcel of `data_size` towards the other side of
  // the route may exceed the specified `limits` on the receiving end.
  bool WouldOutboundParcelExceedLimits(size_t data_size,
                                       const IpczPutLimits& limits);

  // Returns true iff accepting an inbound parcel of `data_size` would cause
  // this router's inbound parcel queue to exceed limits specified by `limits`.
  bool WouldInboundParcelExceedLimits(size_t data_size,
                                      const IpczPutLimits& limits);

  // Attempts to send an outbound parcel originating from this Router. Called
  // only as a direct result of a Put() call on the router's owning portal.
  IpczResult SendOutboundParcel(absl::Span<const uint8_t> data,
                                Parcel::PortalVector& portals,
                                std::vector<os::Handle>& os_handles);

  // Closes this side of the Router's own route. Only called on a Router to
  // which a Portal is currently attached, and only by that Portal.
  void CloseRoute();

  // Uses `link` as this Router's new outward link. This is the primary link on
  // which the router transmits parcels and control messages directed toward the
  // other side of its route.
  void SetOutwardLink(mem::Ref<RouterLink> link);

  // Provides the Router with a new inward link to which it should forward all
  // inbound parcels received from its outward link. The Router may also forward
  // outbound parcels received from the new inward link to the outward link.
  void BeginProxying(const PortalDescriptor& descriptor,
                     mem::Ref<RouterLink> link);

  // Finalizes this Router's proxying responsibilities in either direction. Once
  // the proxy has forwarded any inbound parcels up to (but not including)
  // `inward_sequence_length` over to its inward link, and it has forwarded any
  // outbound parcels up to but not including `outward_sequence_length` to its
  // outward link, it will destroy itself.
  bool StopProxying(SequenceNumber inward_sequence_length,
                    SequenceNumber outward_sequence_length);

  // Accepts a parcel routed here from `link` via `routing_id`, which is
  // determined to be either an inbound or outbound parcel based on the active
  // links this Router has at its disposal.
  bool AcceptParcelFrom(NodeLink& link, RoutingId routing_id, Parcel& parcel);

  // Accepts an inbound parcel routed here from some other Router. The parcel
  // is queued here and may either be made available for retrieval by a portal,
  // or (perhaps immediately) forwarded further along the route via this
  // Router's inward link.
  bool AcceptInboundParcel(Parcel& parcel);

  // Accepts an outbound parcel here from some other Router. The parcel is
  // queued for eventual (and possibly immediate) transmission over the Router's
  // outward link.
  bool AcceptOutboundParcel(Parcel& parcel);

  // Accepts notification that one `side` of this route has been closed.
  // The closed side of the route has transmitted all parcels up to but not
  // including the sequence number `sequence_length`.
  void AcceptRouteClosure(Side side, SequenceNumber sequence_length);

  // Retrieves the next available inbound parcel from this Router, if present.
  IpczResult GetNextIncomingParcel(void* data,
                                   uint32_t* num_bytes,
                                   IpczHandle* portals,
                                   uint32_t* num_portals,
                                   IpczOSHandle* os_handles,
                                   uint32_t* num_os_handles);
  IpczResult BeginGetNextIncomingParcel(const void** data,
                                        uint32_t* num_data_bytes,
                                        uint32_t* num_portals,
                                        uint32_t* num_os_handles);
  IpczResult CommitGetNextIncomingParcel(uint32_t num_data_bytes_consumed,
                                         IpczHandle* portals,
                                         uint32_t* num_portals,
                                         IpczOSHandle* os_handles,
                                         uint32_t* num_os_handles);

  IpczResult AddTrap(std::unique_ptr<Trap> trap);
  IpczResult ArmTrap(Trap& trap,
                     IpczTrapConditionFlags& satistfied_conditions,
                     IpczPortalStatus* status);
  IpczResult RemoveTrap(Trap& trap);

  mem::Ref<Router> Serialize(PortalDescriptor& descriptor);
  static mem::Ref<Router> Deserialize(const PortalDescriptor& descriptor);

  bool InitiateProxyBypass(NodeLink& requesting_node_link,
                           RoutingId requesting_routing_id,
                           const NodeName& proxy_peer_node_name,
                           RoutingId proxy_peer_routing_id,
                           absl::uint128 bypass_key);
  bool BypassProxyTo(mem::Ref<RouterLink> new_peer,
                     absl::uint128 bypass_key,
                     SequenceNumber proxy_outward_sequence_length);

 private:
  friend class LocalRouterLink;

  struct RouterSide {
    RouterSide();
    RouterSide(const RouterSide&) = delete;
    RouterSide& operator=(const RouterSide&) = delete;
    ~RouterSide();

    IncomingParcelQueue parcels;
    mem::Ref<RouterLink> link;
    mem::Ref<RouterLink> decaying_link;
    absl::optional<SequenceNumber> decaying_link_sequence_length;
    bool closure_propagated = false;
  };

  ~Router() override;

  void FlushParcels();

  const Side side_;

  absl::Mutex mutex_;
  RouterSide inward_ ABSL_GUARDED_BY(mutex_);
  RouterSide outward_ ABSL_GUARDED_BY(mutex_);
  bool outward_transmission_paused_ = false;
  SequenceNumber outward_sequence_length_ = 0;
  IpczPortalStatus status_ ABSL_GUARDED_BY(mutex_) = {sizeof(status_)};
  TrapSet traps_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTER_H_
