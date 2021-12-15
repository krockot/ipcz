// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// no-include-guard-because-multiply-included

// This file defines the internal messages which can be sent on a NodeLink
// between two ipcz nodes.

// This needs to be incremented any time changes are made to these definitions.
IPCZ_PROTOCOL_VERSION(0)

// Initial greeting sent by every node via the ConnectNode() API.
IPCZ_MSG_NO_REPLY(Connect, IPCZ_MSG_ID(0), IPCZ_MSG_VERSION(0))
  // The name of the sending node. Names should be randomly generated once at
  // the start of a node's lifetime. They are large and random for global
  // uniqueness, not for security reasons.
  IPCZ_MSG_PARAM(NodeName, name)

  // The highest protocol version known and desired by the sender.
  IPCZ_MSG_PARAM(uint32_t, protocol_version)

  // The number of initial portals assumed on the sender's end of the
  // connection. If there is a mismatch between the number sent by each node on
  // an initial connection, the node which sent the larger number should behave
  // as if its excess portals have observed peer closure.
  IPCZ_MSG_PARAM(uint32_t, num_initial_portals)

  // An optional handle to a shared memory object which can be used to allocate
  // chunks of shared state.
  //
  // TODO: decide on resolution here if both/neither ends provide a handle. for
  // now, ConnectNode() only goes between broker and non-broker, and we adopt
  // the convention that the broker always sends a handle and the non-broker
  // never does. will need something better to support non-broker to non-broker
  // ConnectNode().
  //
  // one idea would be to leave the transport in a state not suitable for portal
  // operation until someone provides a memory object (if neither does); and to
  // introduce a separate message for adding a new link buffer, which we'll need
  // anyway. also if both send a memory handle, we could e.g. assign one as
  // the primary and the other as an auxilliary buffer, based on the relative
  // ordering of the `name` given on each side; but that requires both sides
  // waiting for a Connect from the other side before parcels can flow in either
  // direction, whereas otherwise at least one side could begin operating
  // immediately.
  IPCZ_MSG_HANDLE_OPTIONAL(link_state_memory)
IPCZ_MSG_END()

// Notifies a node that the route has been closed on one side. If this arrives
// at a router from an inward facing or bridge link, it implicitly pertains to
// the router's own side of the route. Otherwise it indicates that the other
// side of the route has been closed. In either case, `sequence_length` is the
// total number of parcels transmitted from the closed side before closing.
IPCZ_MSG_NO_REPLY(RouteClosed, IPCZ_MSG_ID(2), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
  IPCZ_MSG_PARAM(SequenceNumber, sequence_length)
IPCZ_MSG_END()

// Sent by a non-broker node to a broker node. Requests that the broker provide
// a new NodeLink to both the sender and the node identified by `name`, linking
// the two nodes together and allowing them to communicate directly. This
// message has no reply.
//
// If the broker does not know the node named `name`, it will send an
// IntroduceNode message back to the sender with empty handles, indicating
// failure. Otherwise it will send an IntroduceNode message to both the sender
// and the node identified by `name`, with complementary transport descriptors
// attached to each, and an os::Memory handle in `link_state_memory` which each
// side can use to map a shared (zero-initialized) NodeLinkState.
IPCZ_MSG_NO_REPLY(RequestIntroduction, IPCZ_MSG_ID(3), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(NodeName, name)
IPCZ_MSG_END()

// Informs the recipient that its outward peer is a proxy which has negotiated
// its own turn to decay and be bypassed along the route. This provides the
// recipient with enough information to contact the proxy's own outward peer
// directly in order to request that its link to the proxy be bypassed with a
// direct link to the recipient.
IPCZ_MSG_NO_REPLY(InitiateProxyBypass, IPCZ_MSG_ID(5), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
  IPCZ_MSG_PARAM(uint64_t, reserved0)
  IPCZ_MSG_PARAM(NodeName, proxy_peer_name)
  IPCZ_MSG_PARAM(RoutingId, proxy_peer_routing_id)
IPCZ_MSG_END()

// Simultaneously informs the recipient that its outward link is connected to a
// proxying router, and requests that this link be replaced immediately
// with a more direct link to the proxy's inward peer, who is the sender of this
// message.
//
// By the time this is message is sent from a proxy's inward peer to the proxy's
// outward peer, the proxy must have already informed the outward peer of its
// inward peer's NodeName. The outward peer will then be able to reliably
// authenticate the source of this BypassProxy request.
IPCZ_MSG_NO_REPLY(BypassProxy, IPCZ_MSG_ID(6), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(NodeName, proxy_name)
  IPCZ_MSG_PARAM(RoutingId, proxy_routing_id)
  IPCZ_MSG_PARAM(RoutingId, new_routing_id)
  IPCZ_MSG_PARAM(SequenceNumber, proxied_outbound_sequence_length)
IPCZ_MSG_END()

// Equivalent to BypassProxy, but used only when the proxy and its outward peer
// live on the same node. In this case there's no need for the proxy's inward
// peer to authenticate any request to the outward peer, because the outward
// peer is made aware of made aware of the bypass operation as soon as the
// proxy initiates it.
//
// When the proxy's inward peer receives this message, it may immediately begin
// decaying its link on `routing_id` to the proxy, replacing it with a new link
// link (to the same node) on `new_routing_id` to the proxy's outward peer.
//
// `sequence_length` indicates the length of the sequence already sent or
// in-flight over `routing_id` from the proxy, and the recipient of this message
// must send a StopProxyingToLocalPeer back to the proxy with the length its own
// outbound sequence had at the time it began decaying the link on `routing_id`.
IPCZ_MSG_NO_REPLY(BypassProxyToSameNode, IPCZ_MSG_ID(7), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
  IPCZ_MSG_PARAM(RoutingId, new_routing_id)
  IPCZ_MSG_PARAM(SequenceNumber, sequence_length)
IPCZ_MSG_END()

// Informs the recipient that the portal on `routing_id` for this NodeLink can
// cease to exist once it has received and forwarded parcels up to the
// specified sequence length in each direction.
IPCZ_MSG_NO_REPLY(StopProxying, IPCZ_MSG_ID(8), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
  IPCZ_MSG_PARAM(SequenceNumber, inbound_sequence_length)
  IPCZ_MSG_PARAM(SequenceNumber, outbound_sequence_length)
IPCZ_MSG_END()

// Informs the recipient that it has been bypassed by the sender in favor of a
// direct route to the sender's local outward peer. This is essentially a reply
// to BypassProxyToSameNode.
IPCZ_MSG_NO_REPLY(StopProxyingToLocalPeer, IPCZ_MSG_ID(9), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
  IPCZ_MSG_PARAM(SequenceNumber, sequence_length)
IPCZ_MSG_END()

// Informs the recipient that its decaying outward link (implicitly going to a
// decaying proxy) will only receive inbound parcels up to but not including the
// given `sequence_length`.
IPCZ_MSG_NO_REPLY(ProxyWillStop, IPCZ_MSG_ID(10), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
  IPCZ_MSG_PARAM(SequenceNumber, sequence_length)
IPCZ_MSG_END()

IPCZ_MSG_NO_REPLY(DecayUnblocked, IPCZ_MSG_ID(11), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
IPCZ_MSG_END()

// Requests that the receiving Router log a description of itself and forward
// this request along the same direction in which it was received.
IPCZ_MSG_NO_REPLY(LogRouteTrace, IPCZ_MSG_ID(12), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
IPCZ_MSG_END()
