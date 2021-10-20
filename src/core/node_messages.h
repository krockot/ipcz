// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/message.h"

namespace ipcz {
namespace core {
namespace msg {

// Requests an os::Channel connected to the broker in the recipient's cluster of
// connected nodes.
MSG_START(RequestBrokerLink, MSG_ID(0), MSG_VERSION(0))
MSG_END()

}  // namespace msg
}  // namespace core
}  // namespace ipcz
