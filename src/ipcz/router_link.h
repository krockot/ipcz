// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_ROUTER_LINK_H_
#define IPCZ_SRC_IPCZ_ROUTER_LINK_H_

#include <cstddef>
#include <string>
#include <utility>

#include "ipcz/fragment_ref.h"
#include "ipcz/ipcz.h"
#include "ipcz/link_type.h"
#include "ipcz/node_name.h"
#include "ipcz/router_link_state.h"
#include "ipcz/sequence_number.h"
#include "ipcz/sublink_id.h"
#include "util/ref_counted.h"

namespace ipcz {

class NodeLink;
class Parcel;
class Router;

// A RouterLink represents one endpoint of a link between two Routers. Each
// conceptual link along a route has two RouterLink objects associated with it,
// one on either side of the link, connecting the link to a Router on that side.
class RouterLink : public RefCounted {
 public:
  using Pair = std::pair<Ref<RouterLink>, Ref<RouterLink>>;

  // Indicates what type of link this is. See LinkType documentation.
  virtual LinkType GetType() const = 0;

  // Returns a reference to the local Router on the other side of this link
  // (connected to a corresponding RouterLink on the other side), if and only if
  // that Router is local to the same node as the Router on this side.
  virtual Ref<Router> GetLocalTarget() = 0;

  // Indicates whether this RouterLink is a remote link which runs over the
  // identified sublink on the identified NodeLink.
  virtual bool IsRemoteLinkTo(const NodeLink& node_link,
                              SublinkId sublink) const = 0;

  // Signals that this side of the link is in a stable state suitable for one
  // side or the other to lock the link, either for bypass or closure
  // propagation. Only once both sides are marked stable can either side lock
  // the link with TryLock* methods below.
  virtual void MarkSideStable() = 0;

  // Attempts to lock the link for the router on this side to coordinate its own
  // bypass. Returns true if and only if successful, meaning the link is locked
  // and it's safe for the router who locked it to coordinate its own bypass by
  // providing its inward and outward peers with a new central link over which
  // they may communicate directly.
  //
  // On success, `bypass_request_source` is also stashed in this link's shared
  // state so that the other side of the link can authenticate a bypass request
  // coming from that node. This parameter may be omitted if the bypass does not
  // not require authentication, e.g. because the requesting inward peer's node
  // is the same as the proxy's own node, or that of the proxy's current outward
  // peer.
  virtual bool TryLockForBypass(const NodeName& bypass_request_source = {}) = 0;

  // Attempts to lock the link for the router on this side to propagate route
  // closure toward the other side. Returns true if and only if successful,
  // meaning no further bypass operations will proceed on the link.
  virtual bool TryLockForClosure() = 0;

  // Unlocks a link previously locked by one of the TryLock* methods above.
  virtual void Unlock() = 0;

  // Asks the other side to flush its router if and only if the side marked
  // itself as waiting for both sides of the link to become stable, and both
  // sides of the link are stable.
  virtual void FlushOtherSideIfWaiting() = 0;

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

  // Passes a notification to the Router on the other side of this link to
  // indicate that its route has been broken by a disconnection somewhere on
  // the caller's side of the link.
  virtual void AcceptRouteDisconnection() = 0;

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
  virtual void RequestProxyBypassInitiation(const NodeName& to_new_peer,
                                            SublinkId proxy_peer_sublink) = 0;

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
  // provides the target Router with a new sublink it can use to communicate
  // with the that peer directly rather than going through the proxy.
  //
  // `sequence_length` is the final length of the parcel sequence that will be
  // transmitted from the calling proxy to the receiving Router. The receiving
  // Router must assume that any subsequent parcels from the other half of the
  // route will instead arrive via the new SublinkId. The receiver is expected
  // to call back to the sender with StopProxyingToLocalPeer().
  virtual void BypassProxyToSameNode(
      SublinkId new_sublink,
      FragmentRef<RouterLinkState> new_link_state,
      SequenceNumber proxy_inbound_sequence_length) = 0;

  // Essentially a reply to BypassProxyToSameNode, this informs the receiving
  // proxy Router of the final length of the parcel sequence that will be
  // transmitted to it by the sender. All subsequent parcels from that half of
  // the route will instead be transmitted to the proxy's own outward, local
  // peer.
  virtual void StopProxyingToLocalPeer(
      SequenceNumber proxy_outbound_sequence_length) = 0;

  // If the memory location of this link's shared state is known on this side
  // but not the other side, this shares it with the other side.
  virtual void ShareLinkStateMemoryIfNecessary() = 0;

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

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_ROUTER_LINK_H_
