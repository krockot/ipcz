// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_ROUTER_H_
#define IPCZ_SRC_IPCZ_ROUTER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "ipcz/ipcz.h"
#include "ipcz/link_type.h"
#include "ipcz/node_name.h"
#include "ipcz/parcel.h"
#include "ipcz/parcel_queue.h"
#include "ipcz/route_edge.h"
#include "ipcz/sequence_number.h"
#include "ipcz/sublink_id.h"
#include "ipcz/trap_set.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/os_handle.h"
#include "util/ref_counted.h"

namespace ipcz {

class LocalRouterLink;
class NodeLink;
struct RouterDescriptor;
class RouterLink;
class RemoteRouterLink;

// The Router is the main primitive responsible for routing parcels between ipcz
// portals. Each portal directly controls a terminal router along its route, and
// all routes stabilize to eventually consist of only two interconnected
// terminal routers.
//
// When a portal moves, its side of the route is extended by creating a new
// terminal router at the portal's new location. The previous terminal router
// remains as a proxying hop to be phased out eventually.
class Router : public RefCounted {
 public:
  using Pair = std::pair<Ref<Router>, Ref<Router>>;

  // Helper to provide limited access to the Router's state while holding its
  // internal lock. Used by traps to keep the router's `status_` stable briefly
  // while deciding whether to trigger an event.
  class ABSL_SCOPED_LOCKABLE Locked {
   public:
    explicit Locked(Router& router) ABSL_EXCLUSIVE_LOCK_FUNCTION(router.mutex_)
        : router_(router) {
      router_.mutex_.Lock();
    }

    ~Locked() ABSL_UNLOCK_FUNCTION() { router_.mutex_.Unlock(); }

    const IpczPortalStatus& status() const {
      router_.mutex_.AssertHeld();
      return router_.status_;
    }

   private:
    Router& router_;
  };

  Router();

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
  // `this` and `other`.
  bool HasLocalPeer(const Ref<Router>& other);

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
                                Parcel::ObjectVector& objects,
                                std::vector<OSHandle>& os_handles);

  // Closes this side of the Router's own route. Only called on a Router to
  // which a Portal is currently attached, and only by that Portal.
  void CloseRoute();

  // Attempts to merge this terminal Router with another. Merged Routers coexist
  // as local peers which forward parcels between each other, effectively
  // bridging two independent routes. Once both routers have an outward link to
  // the other side of their route, we decay the pair into a single bypass link
  // between each router's outward peer and eventually phase out the local link
  // and the merged routers altogether.
  IpczResult Merge(Ref<Router> other);

  // Uses `link` as this Router's new outward link. This is the primary link on
  // which the router transmits parcels and control messages directed toward the
  // other side of its route. Must only be called on a Router which has no
  // outward link.
  //
  // NOTE: This is NOT safe to call when the other side of the link is already
  // in active use by another Router, as `this` Router may already be in a
  // transitional state and must be able to block decay around `link` from
  // within this call.
  void SetOutwardLink(Ref<RouterLink> link);

  // Finalizes this Router's proxying responsibilities in either direction. Once
  // the proxy has forwarded any inbound parcels up to (but not including)
  // `inbound_sequence_length` over to its inward link, and it has forwarded any
  // outbound parcels up to the current outbound sequence length to its outward
  // link, it will destroy itself.
  bool StopProxying(SequenceNumber proxy_inbound_sequence_length,
                    SequenceNumber proxy_outbound_sequence_length);

  // Accepts an inbound parcel routed here from some other Router. The parcel
  // is queued here and may either be made available for retrieval by a portal,
  // or (perhaps immediately) forwarded further along the route via this
  // Router's inward link.
  bool AcceptInboundParcel(Parcel& parcel);

  // Accepts an outbound parcel here from some other Router. The parcel is
  // queued for eventual (and possibly immediate) transmission over the Router's
  // outward link.
  bool AcceptOutboundParcel(Parcel& parcel);

  // Accepts notification that the outward endpoint of the route has been
  // closed, and that the closed side of the route transmitted a total of
  // `sequence_length` parcels before closing.
  bool AcceptRouteClosureFrom(LinkType link_type,
                              SequenceNumber sequence_length);

  // Accepts notification that the route has been unexpectedly disconnected
  // somewhere at or beyond one of this router's links of type `link_type`.
  void AcceptRouteDisconnectionFrom(LinkType link_type);

  // Retrieves the next available inbound parcel from this Router, if present.
  IpczResult GetNextIncomingParcel(IpczGetFlags flags,
                                   void* data,
                                   uint32_t* num_bytes,
                                   IpczHandle* handles,
                                   uint32_t* num_handles,
                                   IpczOSHandle* os_handles,
                                   uint32_t* num_os_handles);
  IpczResult BeginGetNextIncomingParcel(const void** data,
                                        uint32_t* num_data_bytes,
                                        uint32_t* num_handles,
                                        uint32_t* num_os_handles);
  IpczResult CommitGetNextIncomingParcel(uint32_t num_data_bytes_consumed,
                                         absl::Span<IpczHandle> handles,
                                         absl::Span<IpczOSHandle> os_handles);

  IpczResult Trap(const IpczTrapConditions& conditions,
                  IpczTrapEventHandler handler,
                  uint64_t context,
                  IpczTrapConditionFlags* satisfied_condition_flags,
                  IpczPortalStatus* status);

  // Serializes a description of a new Router to be introduced on a receiving
  // node as an extension of this route. Also makes any necessary state changes
  // changes to prepare `this` (and its local peer if applicable) for the new
  // new remote router's introduction.
  void SerializeNewRouter(NodeLink& to_node_link, RouterDescriptor& descriptor);

  // Finalizes the router's new state (and its local peer's new state if
  // applicable) after a RouterDescriptor serialized by SerializeNewRouter()
  // above has been transmitted to its destination. This establishes links to
  // the newly created router, as those links were not safe to establish prior
  // to transmission of the new descriptor.
  void BeginProxyingToNewRouter(NodeLink& to_node_link,
                                const RouterDescriptor& descriptor);

  // Deserializes a new Router from `descriptor` received over `from_node_link`.
  static Ref<Router> Deserialize(const RouterDescriptor& descriptor,
                                 NodeLink& from_node_link);

  bool InitiateProxyBypass(NodeLink& requesting_node_link,
                           SublinkId requesting_sublink,
                           const NodeName& proxy_peer_node_name,
                           SublinkId proxy_peer_sublink);
  bool BypassProxyWithNewRemoteLink(
      Ref<RemoteRouterLink> new_peer,
      SequenceNumber proxy_outbound_sequence_length);
  bool BypassProxyWithNewLinkToSameNode(
      Ref<RouterLink> new_peer,
      SequenceNumber proxy_inbound_sequence_length);
  bool StopProxyingToLocalPeer(SequenceNumber proxy_outbound_sequence_length);
  bool OnProxyWillStop(SequenceNumber proxy_inbound_sequence_length);

  // Logs a detailed description of this router for debugging.
  void LogDescription();

  // Logs a detailed description of this router and every router along the
  // route from this one, in the direction of the terminal router on the
  // other side of the route.
  void LogRouteTrace();

  // Logs a description of this router and forwards the trace along the opposite
  // direction from whence it was received.
  void AcceptLogRouteTraceFrom(LinkType link_type);

  // Flushes any inbound or outbound parcels to be proxied, as well as any route
  // closure notifications. If the result of the flush is that one or more
  // RouterLinks is no longer necessary, they will be deactivated here. As a
  // result, Flush() may delete `this` if it happens to cause this Router's last
  // reference to be dropped.
  void Flush(bool force_bypass_attempt = false);

  // Notifies this router that the given NodeLink `link` was been disconnected
  // while its sublink `sublink` was bound to this router.
  void NotifyLinkDisconnected(const NodeLink& link, SublinkId sublink);

 private:
  friend class LocalRouterLink;
  friend class NodeLink;

  ~Router() override;

  // Attempts to mark this side of the outward link as decaying and, if
  // successful, asks its inward peer to initiate our bypass along the route.
  bool MaybeInitiateSelfRemoval();

  // Attempts to mark both sides of the bridge (this router's outward link and
  // it the local bridge peer's outward link) as decaying and, if successful,
  // initiates creation of a direct bypass link between each bridge router's
  // outward peer.
  void MaybeInitiateBridgeBypass();

  // Serializes the description of a new Router to be introduced on another node
  // to extend the route as an inward peer of `this` router. `local_peer` must
  // be the this router's locally linked outward peer at the time of the call.
  //
  // If the local link between this router and `local_peer` is not in an
  // appropriate state to support this optimized serialization path, this will
  // return false without modifying the state of either router. In such cases,
  // callers should fall back on the default serialization path defined by
  // SerializeNewRouterAndConfigureProxy().
  bool SerializeNewRouterWithLocalPeer(NodeLink& to_node_link,
                                       RouterDescriptor& descriptor,
                                       Ref<Router> local_peer);

  // Serializes the description of a new Router to be introduced on another node
  // to extend the route as an inward peer of `this` router. This router is
  // given a new (temporarily paused) inward peripheral link to the new router.
  // Once the descriptor is transmitted, links are unpaused and this router's
  // outward link may become eligible for decay. If `can_bypass_proxy` is true,
  // it is safe for the serialized router to begin bypassing us immediately upon
  // deserialization.
  void SerializeNewRouterAndConfigureProxy(NodeLink& to_node_link,
                                           RouterDescriptor& descriptor,
                                           bool initiate_proxy_bypass);

  absl::Mutex mutex_;

  // The current computed portal status to be reflected by a portal controlling
  // this router, iff this is a terminal router.
  IpczPortalStatus status_ ABSL_GUARDED_BY(mutex_) = {sizeof(status_)};

  // A set of traps installed via a controlling portal where applicable. These
  // traps are notified about any interesting state changes within the router.
  TrapSet traps_ ABSL_GUARDED_BY(mutex_);

  // An edge connecting this router outward to another router closer to the
  // terminal router on the opposite side of the route.
  RouteEdge outward_edge_ ABSL_GUARDED_BY(mutex_);

  // Parcels transmitted directly from this router (if sent by a controlling
  // portal) or received from an inward peer which sent them outward toward us.
  // These parcels are forwarded along `outward_edge_` whenever feasible.
  ParcelQueue outbound_parcels_ ABSL_GUARDED_BY(mutex_);

  // An edge connecting this router inward to another router closer to the
  // terminal router on this side of the route. Null if and only if this is a
  // terminal router.
  absl::optional<RouteEdge> inward_edge_ ABSL_GUARDED_BY(mutex_);

  // Parcels received from the other end of the route. If this is a terminal
  // router, these may be retrieved by the application via a controlling portal.
  // Otherwise they will be forwarded over `inward_edge_` whenever feasible.
  ParcelQueue inbound_parcels_ ABSL_GUARDED_BY(mutex_);

  // Link used only if this router has been merged with another, which is a
  // relatively rare operation. This link proxies parcels between two separate
  // routes and uses a specialized bypass operation to eliminate itself over
  // time. This is essentially treated as an inward edge, but it also acts as
  // a boundary between successive routes, allowing each bridged route to be
  // reduced independently in parallel before eliminating the bridge itself.
  std::unique_ptr<RouteEdge> bridge_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_ROUTER_H_
