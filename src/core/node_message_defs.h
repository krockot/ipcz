// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// no-include-guard-because-multiply-included

// This needs to be incremented any time changes are made to these definitions.
IPCZ_PROTOCOL_VERSION(0)

IPCZ_MSG_WITH_REPLY(InviteNode, IPCZ_MSG_ID(0), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(uint32_t, protocol_version)
  IPCZ_MSG_PARAM(NodeName, source_name)
  IPCZ_MSG_PARAM(NodeName, target_name)
  IPCZ_MSG_PARAM(RouteId, route)
  IPCZ_MSG_HANDLE_REQUIRED(node_link_state_memory)
  IPCZ_MSG_HANDLE_REQUIRED(portal_link_state_memory)
IPCZ_MSG_END()

IPCZ_MSG_REPLY(InviteNode, IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(uint32_t, protocol_version)
  IPCZ_MSG_PARAM(bool, accepted : 1)
IPCZ_MSG_END()

IPCZ_MSG_NO_REPLY(PeerClosed, IPCZ_MSG_ID(2), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RouteId, route)
  IPCZ_MSG_PARAM(SequenceNumber, sequence_length)
IPCZ_MSG_END()

IPCZ_MSG_NO_REPLY(RequestIntroduction, IPCZ_MSG_ID(3), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(NodeName, name)
IPCZ_MSG_END()

IPCZ_MSG_NO_REPLY(IntroduceNode, IPCZ_MSG_ID(4), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(NodeName, name)
  IPCZ_MSG_HANDLE_OPTIONAL(channel)
  IPCZ_MSG_HANDLE_OPTIONAL(link_state_memory)
IPCZ_MSG_END()

IPCZ_MSG_NO_REPLY(RedirectRoute, IPCZ_MSG_ID(5), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(NodeName, predecessor_name)
  IPCZ_MSG_PARAM(RouteId, predecessor_route)
  IPCZ_MSG_PARAM(RouteId, new_route)
  IPCZ_MSG_PARAM(Side, sender_side)
  IPCZ_MSG_HANDLE_REQUIRED(new_link_state_memory)
  IPCZ_MSG_PARAM(absl::uint128, key)
IPCZ_MSG_END()

IPCZ_MSG_NO_REPLY(StopProxying, IPCZ_MSG_ID(6), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RouteId, route)
  IPCZ_MSG_PARAM(SequenceNumber, sequence_length)
IPCZ_MSG_END()
