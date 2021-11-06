// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CORE_NODE_MESSAGES_H_
#define CORE_NODE_MESSAGES_H_

#include <cstdint>

#include "core/message_internal.h"
#include "core/node_name.h"
#include "core/route_id.h"
#include "core/sequence_number.h"
#include "ipcz/ipcz.h"
#include "os/channel.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {
namespace msg {

// This file is used to push message definitions through the preprocessor to
// emit message structure declarations. See node_message_defs.h for the actual
// message definitions.

#pragma pack(push, 1)

#include "core/message_macros/message_params_declaration_macros.h"
#include "core/node_message_defs.h"

#include "core/message_macros/undef_message_macros.h"

#include "core/message_macros/message_handle_data_declaration_macros.h"
#include "core/node_message_defs.h"

#include "core/message_macros/undef_message_macros.h"

#include "core/message_macros/message_handle_declaration_macros.h"
#include "core/node_message_defs.h"

#include "core/message_macros/undef_message_macros.h"

#include "core/message_macros/message_declaration_macros.h"
#include "core/node_message_defs.h"

#include "core/message_macros/undef_message_macros.h"

#pragma pack(pop)

// hack
static constexpr uint8_t kAcceptParcelId = 1;

}  // namespace msg
}  // namespace core
}  // namespace ipcz

#endif  // CORE_NODE_MESSAGES_H_
