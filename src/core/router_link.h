// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTER_LINK_H_
#define IPCZ_SRC_CORE_ROUTER_LINK_H_

#include <cstddef>
#include <string>
#include <utility>

#include "core/link_side.h"
#include "core/node_name.h"
#include "core/route_side.h"
#include "core/routing_id.h"
#include "core/sequence_number.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/numeric/int128.h"

namespace ipcz {
namespace core {

class NodeLink;
class Parcel;
class Router;
struct RouterLinkState;

// A RouterLink represents one endpoint of a link between two Routers. Each
// RouterLink is arbitrarily assigned a LinkSide at construction time, and the
// RouterLink on the other side of the link is assigned the opposite LinkSide.
//
// In addition to facilitating communication between linked Routers for route
// inspection and mutation, each RouterLink shares some shared state (a
// RouterLinkState structure) with a RouterLink on the opposite LinkSide of the
// same link. The LinkSide property of each RouterLink is used as a convention
// to establish each corresponding Router's own responsibility for bits in the
// RouterLinkState.
class RouterLink : public mem::RefCounted {
 public:
  using Pair = std::pair<mem::Ref<RouterLink>, mem::Ref<RouterLink>>;

  // Returns the side of the link (side A or B) on which this RouterLink object
  // falls. Neither side has special meaning: this is used as a convention for
  // each side of a link to agree on a unique relative identity where it's
  // useful to do so. For example in state shared by both sides of the link,
  // data or state changes may be scoped to one side of the link or the other.
  virtual LinkSide GetLinkSide() const = 0;

  // Indicates which side of the route the other side of the link is connected
  // to, relative to Router on this side of the link.
  virtual RouteSide GetTargetRouteSide() const = 0;

  // Returns a reference to the shared RouterLinkState used by both sides of the
  // same link. The other RouterLink using this state may be on another thread
  // or in another process, and it must be constructed with a LinkSide property
  // opposite of this RouterLink.
  virtual RouterLinkState& GetLinkState() = 0;

  // Returns a reference to the local Router on the other side of this link
  // (connected to a corresponding RouterLink on the other side), if and only if
  // that Router is local to the same node as the Router on this side.
  virtual mem::Ref<Router> GetLocalTarget() = 0;

  // Returns the NodeLink and RoutingId used by this RouterLink to communicate
  // with the Router on the other side of this link, if and only if that Router
  // lives on a different node from the Router on this side.
  virtual bool IsRemoteLinkTo(NodeLink& node_link, RoutingId routing_id) = 0;

  // Indicates an imperfect estimation of whether or not putting a parcel with
  // `data_size` bytes of data onto this link may ultimately cause the eventual
  // (terminal) destination Router to exceed queue limits specified in `limits`.
  virtual bool WouldParcelExceedLimits(size_t data_size,
                                       const IpczPutLimits& limits) = 0;

  // Passes a parcel to the Router on the other side of this link for further
  // routing.
  virtual void AcceptParcel(Parcel& parcel) = 0;

  // Passes a notification to the Router on the other side of this link to
  // indicate that the given `route_side` of the route itself has been closed.
  // `route_side` is relative to the calling Router. So for example if this is a
  // a transverse link (GetTargetRouteSide() is RouteSide::kOther) and
  // `route_side` is given here as RouteSide::kSame, then the receiving Router
  // will receive the call with `route_side` as RouteSide::kOther, since the
  // "same" side from our perspective is the "other" side from their
  // perspective.
  virtual void AcceptRouteClosure(RouteSide route_side,
                                  SequenceNumber sequence_length) = 0;

  // Requests that the Router on the other side of this link initiate a bypass
  // of the Router on this side of the link. The provided parameters are enough
  // information for the receiving Router to establish a direct connection to
  // this Router's outward peer and to authenticate a request its link to this
  // router with a direct link to the receiving router.
  //
  // The receiver is expected to initiate this bypass by reaching out to
  // `to_new_peer` directly on a NodeLink to that peer. See
  // NodeLink::BypassProxy(); and note that BypassProxy is not a RouterLink
  // method, because there is not yet any RouterLink between the sending and
  // receiving Routers.
  virtual void RequestProxyBypassInitiation(
      const NodeName& to_new_peer,
      RoutingId proxy_peer_routing_id,
      const absl::uint128& bypass_key) = 0;

  // Informs the Router on the other side of this link that it can stop
  // forwarding parcels to its inward peer once it has forwarded up to
  // `inbound_sequence_length` parcels, and that it can stop forwarding to its
  // outward peer once it has forwarded up to `outbound_sequence_length`
  // parcels.
  virtual void StopProxying(SequenceNumber inbound_sequence_length,
                            SequenceNumber outbound_sequence_length) = 0;

  // Informs the Router on the other side of this link that the proxy router it
  // most recently bypassed will stop sending it parcels once the inbound
  // sequence length reaches `sequence_length`, at which point its link to the
  // proxy can be discarded.
  virtual void ProxyWillStop(SequenceNumber sequence_length) = 0;

  // Informs the Router on the other side of this link that its outward link
  // goes to a proxying Router who is ready to be bypassed. The proxy's own
  // outward peer is on the same node as the proxy itself, and this call
  // provides the target Router with a new routing ID it can use to communicate
  // with the that peer directly rather than going through the proxy.
  //
  // `sequence_length` is the final length of the parcel sequence that will be
  // transmitted from the calling proxy to the receiving Router. The receiving
  // Router must assume that any subsequent parcels from the other half of the
  // route will instead arrive via the new RoutingId. The receiver is expected
  // to call back to the sender with StopProxyingToLocalPeer().
  virtual void BypassProxyToSameNode(RoutingId new_routing_id,
                                     SequenceNumber sequence_length) = 0;

  // Essentially a reply to BypassProxyToSameNode, this informs the receiving
  // proxy Router of the final length of the parcel sequence that will be
  // transmitted to it by the sender. All subsequent parcels from that half of
  // the route will instead be transmitted to the proxy's own outward, local
  // peer.
  virtual void StopProxyingToLocalPeer(SequenceNumber sequence_length) = 0;

  // Informs the Router on the other side of this link that the link is now
  // fully ready to support further decay. Sent only if this side of the link
  // was the last side to become ready, and it does not need to decay itself
  // further.
  virtual void DecayUnblocked() = 0;

  // Deactivates the link, ensuring that it no longer facilitates new calls
  // arriving for its associated Router.
  virtual void Deactivate() = 0;

  // Verbosely describes this link for debug logging.
  virtual std::string Describe() const = 0;

  // Asynchronously and verbosely logs a description of every Router along this
  // link, from the Router on the other side of this link, to the terminal
  // Router on the identified route side relative to the calling Router.
  virtual void LogRouteTrace(RouteSide toward_route_side) = 0;

 protected:
  ~RouterLink() override = default;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTER_LINK_H_
