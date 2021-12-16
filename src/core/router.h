// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTER_H_
#define IPCZ_SRC_CORE_ROUTER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "core/decayable_link.h"
#include "core/direction.h"
#include "core/node_name.h"
#include "core/parcel.h"
#include "core/parcel_queue.h"
#include "core/routing_id.h"
#include "core/sequence_number.h"
#include "core/trap.h"
#include "core/trap_set.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class LocalRouterLink;
class NodeLink;
struct RouterDescriptor;
class RouterLink;
class RemoteRouterLink;

class Router : public mem::RefCounted {
 public:
  using Pair = std::pair<mem::Ref<Router>, mem::Ref<Router>>;

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

  // Returns the total number of Routers living in the calling process.
  static size_t GetNumRoutersForTesting();

  // Logs a description of every router instance in the calling process.
  static void DumpAllRoutersForDebugging();

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
  bool HasLocalPeer(const mem::Ref<Router>& other);

  // A stricter version of HasLocalPeer() which requires both routers to be
  // terminal routers with no decaying links.
  bool HasStableLocalPeer(const mem::Ref<Router>& other);

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

  // Attempts to merge this terminal Router with another. Merged Routers coexist
  // as local peers which forward parcels between each other, effectively
  // bridging two independent routes. Once both routers have an outward link to
  // the other side of their route, we decay the pair into a single bypass link
  // between each router's outward peer and eventually phase out the local link
  // and the merged routers altogether.
  IpczResult Merge(mem::Ref<Router> other);

  // Uses `link` as this Router's new outward link. This is the primary link on
  // which the router transmits parcels and control messages directed toward the
  // other side of its route. Must only be called on a Router which has no
  // outward link.
  //
  // NOTE: This is NOT safe to call when the other side of the link is already
  // in active use by another Router, as `this` Router may already be in a
  // transitional state and must be able to block decay around `link` from
  // within this call.
  void SetOutwardLink(mem::Ref<RouterLink> link);

  // Finalizes this Router's proxying responsibilities in either direction. Once
  // the proxy has forwarded any inbound parcels up to (but not including)
  // `inbound_sequence_length` over to its inward link, and it has forwarded any
  // outbound parcels up to the current outbound sequence length to its outward
  // link, it will destroy itself.
  bool StopProxying(SequenceNumber inbound_sequence_length,
                    SequenceNumber outbound_sequence_length);

  // Accepts an inbound parcel routed here from some other Router. The parcel
  // is queued here and may either be made available for retrieval by a portal,
  // or (perhaps immediately) forwarded further along the route via this
  // Router's inward link.
  bool AcceptInboundParcel(Parcel& parcel);

  // Accepts an outbound parcel here from some other Router. The parcel is
  // queued for eventual (and possibly immediate) transmission over the Router's
  // outward link.
  bool AcceptOutboundParcel(Parcel& parcel);

  // Accepts notification that the endpoint in the `source` direction along this
  // route (relative to this Router) has been closed. The closed side of the
  // route has transmitted a total of `sequence_length` parcels.
  void AcceptRouteClosureFrom(Direction source, SequenceNumber sequence_length);

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

  void AddTrap(mem::Ref<Trap> trap);
  void RemoveTrap(Trap& trap);

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
  static mem::Ref<Router> Deserialize(const RouterDescriptor& descriptor,
                                      NodeLink& from_node_link);

  bool InitiateProxyBypass(NodeLink& requesting_node_link,
                           RoutingId requesting_routing_id,
                           const NodeName& proxy_peer_node_name,
                           RoutingId proxy_peer_routing_id);
  bool BypassProxyWithNewRemoteLink(
      mem::Ref<RemoteRouterLink> new_peer,
      SequenceNumber proxy_outbound_sequence_length);
  bool BypassProxyWithNewLinkToSameNode(
      mem::Ref<RouterLink> new_peer,
      SequenceNumber sequence_length_from_proxy);
  bool StopProxyingToLocalPeer(SequenceNumber sequence_length);
  bool OnProxyWillStop(SequenceNumber sequence_length);
  bool OnDecayUnblocked();

  // Logs a detailed description of this router for debugging.
  void LogDescription();

  // Logs a detailed description of this router and every router along the
  // route from this one, in the direction of the terminal router on the
  // other side of the route.
  void LogRouteTrace();

  // Logs a description of this router and forwards the trace along the opposite
  // direction from whence it was received.
  void AcceptLogRouteTraceFrom(Direction source);

 private:
  friend class LocalRouterLink;
  friend class NodeLink;

  ~Router() override;

  // Flushes any inbound or outbound parcels to be proxied, as well as any route
  // closure notifications. If the result of the flush is that one or more
  // RouterLinks is no longer necessary, they will be deactivated here. As a
  // result, Flush() may delete `this` if it happens to cause this Router's last
  // reference to be dropped.
  void Flush();

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
                                       mem::Ref<Router> local_peer);

  // Serializes the description of a new Router to be introduced on another node
  // to extend the route as an inward peer of `this` router. This router is
  // given a new (temporarily paused) inward peripheral link to the new router.
  // Once the descriptor is transmitted, links are unpaused and this router's
  // outward link may become eligible for decay. If `bypass_proxy` is true, it
  // is safe for the serialized router to begin bypassing us immediately upon
  // deserialization.
  void SerializeNewRouterAndConfigureProxy(NodeLink& to_node_link,
                                           RouterDescriptor& descriptor,
                                           bool bypass_proxy);

  absl::Mutex mutex_;
  DecayableLink inward_ ABSL_GUARDED_BY(mutex_);
  DecayableLink outward_ ABSL_GUARDED_BY(mutex_);
  IpczPortalStatus status_ ABSL_GUARDED_BY(mutex_) = {sizeof(status_)};
  TrapSet traps_ ABSL_GUARDED_BY(mutex_);

  // Link used only if this router has been merged with another, which is a
  // relatively rare operation. This link proxies parcels between two separate
  // routes and uses a specialized bypass operation to eliminate itself over
  // time.
  std::unique_ptr<DecayableLink> bridge_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTER_H_
