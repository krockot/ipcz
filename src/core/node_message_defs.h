// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// no-include-guard-because-multiply-included

// This file defines the internal messages which can be sent on a NodeLink
// between two ipcz nodes.

// This needs to be incremented any time changes are made to these definitions.
IPCZ_PROTOCOL_VERSION(0)

// Message sent by the broker on any OS transport given to OpenRemotePortal().
// This establishes one end of a primordial portal pair to another node. The
// other node must call AcceptRemotePortal() so that it can expect and reply to
// this message on its end of the same OS transport.
IPCZ_MSG_WITH_REPLY(InviteNode, IPCZ_MSG_ID(0), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(uint32_t, protocol_version)
  IPCZ_MSG_PARAM(NodeName, source_name)
  IPCZ_MSG_PARAM(NodeName, target_name)
  IPCZ_MSG_PARAM(RouteId, route)
  IPCZ_MSG_HANDLE_REQUIRED(node_link_state_memory)
  IPCZ_MSG_HANDLE_REQUIRED(portal_link_state_memory)
IPCZ_MSG_END()

// Reply sent by AcceptRemotePortal().
IPCZ_MSG_REPLY(InviteNode, IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(uint32_t, protocol_version)
  IPCZ_MSG_PARAM(bool, accepted : 1)
IPCZ_MSG_END()

// Notifies a node that the portal corresponding to `route` on this NodeLink has
// had its peer portal closed. `sequence_length` is the total number of parcels
// sent by the peer portal before closing.
IPCZ_MSG_NO_REPLY(PeerClosed, IPCZ_MSG_ID(2), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RouteId, route)
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
// and the node identified by `name`, with opposite ends of the same os::Channel
// attached to each, and an os::Memory handle in `link_state_memory` which each
// side can use to map a shared (zero-initialized) NodeLinkState.
IPCZ_MSG_NO_REPLY(RequestIntroduction, IPCZ_MSG_ID(3), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(NodeName, name)
IPCZ_MSG_END()

// Sent by a broker node to a non-broker node. Either both `channel` and
// `link_state_memory` are valid or neither is. If not valid, this message
// conveys that the broker does not know of a node named `name`. Otherwise the
// provided handles can be used by the recipient to construct a new NodeLink for
// immediate communication with the named node.
IPCZ_MSG_NO_REPLY(IntroduceNode, IPCZ_MSG_ID(4), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(NodeName, name)
  IPCZ_MSG_HANDLE_OPTIONAL(channel)
  IPCZ_MSG_HANDLE_OPTIONAL(link_state_memory)
IPCZ_MSG_END()

// Simultaneously informs the recipient that one of its PortalLinks is connected
// to a proxying portal, and requests that this PortalLink be replaced
// immediately with a more direct link to the proxy's own destination; which is
// the sender of this message:
//
//  recipient R === [peer link] ==>  proxy P  === [successor link] ==> sender S
//     \                                                               /
//      \__<<<<<<<<<<<<<<<<<[ BypassProxy message ]<<<<<<<<<<<<<<<<___/
//
// In this diagram, portal P was at some point a mutual peer of portal R, and
// portal S did not exist. The active portal at P was then relocated to S on a
// different node, and left in P's place was a half-proxy: a portal that exists
// only to forward parcels to its successor (in this case S).
//
// As part of this operation, P generated a random key and stashed it in the
// shared state of the peer link between R and P. The same key was subsequently
// shared with S (and only S), along with the identity of R and the route ID
// of the peer link between R and P.
//
// S sends this BypassProxy message to R using the same shared secret key,
// allowing R to trust the request and immediately stop using the peer link to
// P, replacing it instead with a new peer link directly to S.
//
// R also sends a StopProxying message to P just before this operation completes
// to inform P of the last in-flight parcel already sent to P and therefore the
// last parcel P needs to accept and forward to S before it can cease to exist.
IPCZ_MSG_NO_REPLY(BypassProxy, IPCZ_MSG_ID(5), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(NodeName, proxy_name)
  IPCZ_MSG_PARAM(RouteId, proxy_route)
  IPCZ_MSG_PARAM(RouteId, new_route)
  IPCZ_MSG_PARAM(Side, sender_side)
  IPCZ_MSG_HANDLE_REQUIRED(new_link_state_memory)
  IPCZ_MSG_PARAM(absl::uint128, key)
IPCZ_MSG_END()

// Informs the recipient that the portal on route `route` for this NodeLink can
// cease to exist once it has received and forwarded to its successor every
// in-flight parcel with a SequenceNumber up to but not including
// `sequence_length`.
IPCZ_MSG_NO_REPLY(StopProxying, IPCZ_MSG_ID(6), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RouteId, route)
  IPCZ_MSG_PARAM(SequenceNumber, sequence_length)
IPCZ_MSG_END()
