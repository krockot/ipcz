// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CORE_NODE_MESSAGES_H_
#define CORE_NODE_MESSAGES_H_

#include <cstdint>

#include "ipcz/buffer_id.h"
#include "ipcz/driver_transport.h"
#include "ipcz/fragment_descriptor.h"
#include "ipcz/handle_descriptor.h"
#include "ipcz/ipcz.h"
#include "ipcz/link_side.h"
#include "ipcz/message_internal.h"
#include "ipcz/node_name.h"
#include "ipcz/router_descriptor.h"
#include "ipcz/sequence_number.h"
#include "ipcz/sublink_id.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/os_handle.h"
#include "util/os_process.h"

namespace ipcz {
namespace msg {

// This file is used to push message definitions through the preprocessor to
// emit message structure declarations. See node_message_defs.h for the actual
// message definitions.

#pragma pack(push, 1)

// clang-format off
#include "ipcz/message_macros/message_params_declaration_macros.h"
#include "ipcz/node_message_defs.h"
#include "ipcz/message_macros/undef_message_macros.h"

#include "ipcz/message_macros/message_declaration_macros.h"
#include "ipcz/node_message_defs.h"
#include "ipcz/message_macros/undef_message_macros.h"
// clang-format on

#pragma pack(pop)

}  // namespace msg
}  // namespace ipcz

#endif  // CORE_NODE_MESSAGES_H_
