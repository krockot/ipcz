// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/message.h"

namespace ipcz {
namespace core {
namespace msg {

enum class LinkTransportType : uint8_t {
  // The default os::Channel type for the current platform.
  kChannel = 0,
};

MSG_BEGIN(RequestBrokerLink, MSG_ID(0))
  MSG_BEGIN_DATA(MSG_VERSION(0))
    MSG_DATA(LinkTransportType, requested_transport_type)
  MSG_END_DATA()
  MSG_NO_HANDLES()

  MSG_REPLY()
    MSG_BEGIN_DATA(MSG_VERSION(0))
      MSG_DATA(LinkTransportType, provided_transport_type)
    MSG_END_DATA()

    MSG_NUM_HANDLES(1)
    MSG_HANDLE_OPTIONAL(0, channel_handle)
  MSG_END()
MSG_END()

}  // namespace msg
}  // namespace core
}  // namespace ipcz
