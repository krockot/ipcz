// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// no-include-guard-because-multiply-included

IPCZ_ENUM_BEGIN(LinkTransportType, uint8_t)
  // No transport. A safe default for unrecognized transport types.
  IPCZ_ENUM_VALUE_DEFAULT(kNone, 0)

  // Indicates presence of an os::Handle with which to construct an instance of
  // the host platform's default v0 os::Channel transport.
  IPCZ_ENUM_VALUE(kDefaultChannel, 1)
IPCZ_ENUM_END()

IPCZ_MSG_WITH_REPLY(RequestBrokerLink, IPCZ_MSG_ID(0), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_DATA(LinkTransportType, requested_transport_type)
IPCZ_MSG_END()

IPCZ_MSG_REPLY(RequestBrokerLink, IPCZ_MSG_VERSION(0))
  IPCZ_MSG_DATA(bool, is_broker : 1)
  IPCZ_MSG_DATA(LinkTransportType, provided_transport_type)
  IPCZ_MSG_HANDLE_OPTIONAL(channel_handle)
IPCZ_MSG_END()
