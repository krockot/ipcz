// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// no-include-guard-because-multiply-included

// This needs to be incremented any time changes are made to these definitions.
IPCZ_PROTOCOL_VERSION(0)

IPCZ_MSG_WITH_REPLY(InviteNode, IPCZ_MSG_ID(0), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(uint32_t, protocol_version)
  IPCZ_MSG_PARAM(PortalAddress, broker_portal)
  IPCZ_MSG_PARAM(PortalAddress, your_portal)
IPCZ_MSG_END()

IPCZ_MSG_REPLY(InviteNode, IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(uint32_t, protocol_version)
  IPCZ_MSG_PARAM(bool, accepted : 1)
IPCZ_MSG_END()
