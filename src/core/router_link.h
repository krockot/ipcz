// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTER_LINK_H_
#define IPCZ_SRC_CORE_ROUTER_LINK_H_

#include <cstddef>
#include <string>
#include <utility>

#include "core/link_type.h"
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

// A RouterLink represents one endpoint of a link between two Routers. Each
// conceptual link along a route has two RouterLink objects associated with it,
// one on other side of the link, connecting the link to a Router on that side.
class RouterLink : public mem::RefCounted {
 public:
  using Pair = std::pair<mem::Ref<RouterLink>, mem::Ref<RouterLink>>;

  // Indicates what type of link this is. See LinkType documentation.
  virtual LinkType GetType() const = 0;

  // Returns a reference to the local Router on the other side of this link
  // (connected to a corresponding RouterLink on the other side), if and only if
  // that Router is local to the same node as the Router on this side.
  virtual mem::Ref<Router> GetLocalTarget() = 0;

  // Returns the NodeLink and RoutingId used by this RouterLink to communicate
  // with the Router on the other side of this link, if and only if that Router
  // lives on a different node from the Router on this side.
  virtual bool IsRemoteLinkTo(NodeLink& node_link, RoutingId routing_id) = 0;

  // Indicates whether this link is in a stable state suitable for decay from
  // one side or the other. This means it exists as a router's outward link, it
  // is a central link, the router has no decaying outward link, and the other
  // side of the link has not already started to decay. A proxying router with a
  // link in this state may attempt to initiate its own bypass and eventual
  // removal from the route, starting with MaybeBeginDecay().
  virtual bool CanDecay() = 0;

  // Signals that this side of the link is in a stable state suitable for decay.
  // Only once both sides of a logical link signal this condition can either
  // side observe CanDecay() as true.
  virtual bool SetSideCanDecay() = 0;

  // Attempts to mark this side of the link for decay. Returns true if and only
  // if successful. This call can only succeed if CanDecay() was true at the
  // time of the call. If successful, CanDecay() will never return true again
  // for either side of this link. This allows the initiating router to
  // coordinate its own bypass with a new mutually outward link between its
  // inward and outward peers, without risk of the outward peer trying to
  // initiate its own bypass at the same time.
  //
  // On success, `bypass_key` (if non-null) is also populated with a randomly
  // generated key which can be given to the router's inward peer as a means of
  // authenticating bypass to the outward peer.
  virtual bool MaybeBeginDecay(absl::uint128* bypass_key = nullptr) = 0;

  // Cancels a decay process that was initiated by either side of the link.
  // Only valid to call when one side or the other has successfully called
  // MaybeBeginDecay(). After completion, CanDecay() may once again be true.
  virtual bool CancelDecay() = 0;

  // Indicates whether this link can be bypassed using the given key. True if
  // and only if the other side of this link has already begun to decay, AND
  // `bypass_key` matches a key generated on that side of the link and provided
  // ahead-of-time to this side of the link.
  virtual bool CanBypassWithKey(const absl::uint128& bypass_key) = 0;

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
  // a central link and `route_side` is given here as RouteSide::kSame, then the
  // receiving Router will receive the call with `route_side` as
  // RouteSide::kOther, since the "same" side from our perspective is the
  // "other" side from their perspective.
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
  virtual void LogRouteTrace() = 0;

 protected:
  ~RouterLink() override = default;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTER_LINK_H_
