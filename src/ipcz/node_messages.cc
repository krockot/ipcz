// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file is used to push message definitions through the preprocessor to
// emit message structure definitions. See node_message_defs.h for the actual
// message definitions.

#include "ipcz/node_messages.h"

namespace ipcz {
namespace msg {

#pragma pack(push, 1)

// clang-format off
#include "ipcz/message_macros/message_params_definition_macros.h"
#include "ipcz/node_message_defs.h"
#include "ipcz/message_macros/undef_message_macros.h"

#include "ipcz/message_macros/message_definition_macros.h"
#include "ipcz/node_message_defs.h"
#include "ipcz/message_macros/undef_message_macros.h"
// clang-format on

#pragma pack(pop)

}  // namespace msg
}  // namespace ipcz
