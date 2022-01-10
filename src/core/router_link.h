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
#include "core/routing_id.h"
#include "core/sequence_number.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"

namespace ipcz {
namespace core {

class NodeLink;
struct NodeLinkAddress;
class Parcel;
class Router;

// A RouterLink represents one endpoint of a link between two Routers. Each
// conceptual link along a route has two RouterLink objects associated with it,
// one on either side of the link, connecting the link to a Router on that side.
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

  // Indicates whether this link is in a stable state suitable for initiating
  // bypass from one side or the other. This means it exists as a router's
  // outward link, it is a central link, the neither side has decaying links,
  // and the other side hasn't already locked the link for bypass. A proxying
  // router with a link in this state may lock the link for bypass by calling
  // TryToLockForBypass(). Once locked, it's safe for the router to coordinate
  // with other routers and have itself bypassed.
  virtual bool CanLockForBypass() = 0;

  // Signals that this side of the link is in a stable state suitable for one
  // side or the other to lock it for bypass. Only once both sides agree to
  // allow bypass can either side observe CanLockForBypass() as true.
  virtual bool SetSideCanSupportBypass() = 0;

  // Attempts to lock the link for the router on this side to coordinate its own
  // bypass. Returns true if and only if successful, meaning the link is locked
  // and it's safe for the router who locked it to coordinate its own bypass by
  // providing its inward and outward peers with a new central link over which
  // they may communicate directly.
  //
  // This call can only succeed if CanLockForBypass() was true at the time of
  // the call. Once locked, CanLockForBypass() will never return true again for
  // either side of this link unless CancelBypassLock() is called.
  //
  // On success, `bypass_request_source` is also stashed in this link's shared
  // state so that the other side of the link can authenticate a bypass request
  // coming from that node. This parameter may be omitted if the bypass does not
  // not require authentication, e.g. because the requesting inward peer's node
  // is the same as the proxy's own node, or that of the proxy's current outward
  // peer.
  virtual bool TryToLockForBypass(
      const NodeName& bypass_request_source = {}) = 0;

  // Unlocks a link previously locked for bypass by a successful call to
  // TryToLockForBypass(). If this returns with success, CanLockForBypass() may
  // once again return true on either side of the link.
  virtual bool CancelBypassLock() = 0;

  // Indicates whether this link can be bypassed by a request from the named
  // node to one side of the link. True if and only if the proxy on the other
  // side of this link has already initiated bypass and `bypass_request_source`
  // matches the NodeName it stored in this link's shared state at that time.
  virtual bool CanNodeRequestBypass(const NodeName& bypass_request_source) = 0;

  // Indicates an imperfect estimation of whether or not putting a parcel with
  // `data_size` bytes of data onto this link may ultimately cause the eventual
  // destination router to any exceed queue limits specified in `limits`.
  virtual bool WouldParcelExceedLimits(size_t data_size,
                                       const IpczPutLimits& limits) = 0;

  // Passes a parcel to the Router on the other side of this link for further
  // routing.
  virtual void AcceptParcel(Parcel& parcel) = 0;

  // Passes a notification to the Router on the other side of this link to
  // indicate that the route endpoint closer to the sender has been closed after
  // sending a total of `sequence_length` parcels.
  virtual void AcceptRouteClosure(SequenceNumber sequence_length) = 0;

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
      RoutingId proxy_peer_routing_id) = 0;

  // Informs the Router on the other side of this link that it can stop
  // forwarding parcels to its inward peer once it has forwarded up to
  // `inbound_sequence_length` parcels, and that it can stop forwarding to its
  // outward peer once it has forwarded up to `outbound_sequence_length`
  // parcels.
  virtual void StopProxying(SequenceNumber proxy_inbound_sequence_length,
                            SequenceNumber proxy_outbound_sequence_length) = 0;

  // Informs the Router on the other side of this link that the proxy router it
  // most recently bypassed will stop sending it parcels once the inbound
  // sequence length reaches `sequence_length`, at which point its link to the
  // proxy can be discarded.
  virtual void ProxyWillStop(SequenceNumber proxy_inbound_sequence_length) = 0;

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
  virtual void BypassProxyToSameNode(
      RoutingId new_routing_id,
      const NodeLinkAddress& new_link_state_address,
      SequenceNumber proxy_inbound_sequence_length) = 0;

  // Essentially a reply to BypassProxyToSameNode, this informs the receiving
  // proxy Router of the final length of the parcel sequence that will be
  // transmitted to it by the sender. All subsequent parcels from that half of
  // the route will instead be transmitted to the proxy's own outward, local
  // peer.
  virtual void StopProxyingToLocalPeer(
      SequenceNumber proxy_outbound_sequence_length) = 0;

  // Informs the Router on the other side of this link that the link is now
  // fully ready to support bypass if necessary. Sent only if this side of the
  // link was the last side to become ready, and this side does not want to
  // initiate bypass itself.
  //
  // TODO: we should be able to use shared state to reliably indicate whether
  // it's worth sending this notification. otherwise we're forced to send it in
  // a bunch of potentially redundant cases.
  virtual void NotifyBypassPossible() = 0;

  // Flushes any necessary state changes from this side of the link to the
  // other.
  virtual void Flush() = 0;

  // Deactivates the link, ensuring that it no longer facilitates new calls
  // arriving for its associated Router.
  virtual void Deactivate() = 0;

  // Verbosely describes this link for debug logging.
  virtual std::string Describe() const = 0;

  // Asynchronously and verbosely logs a description of every router along this
  // link, starting from the Router on the other side of this link and
  // continuing along the route in that direction until a terminal router is
  // reached.
  virtual void LogRouteTrace() = 0;

 protected:
  ~RouterLink() override = default;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTER_LINK_H_
