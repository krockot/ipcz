// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file is used to push message definitions through the preprocessor to
// emit message structure definitions. See node_message_defs.h for the actual
// message definitions.

#include "core/node_messages.h"

namespace ipcz {
namespace core {
namespace msg {

#pragma pack(push, 1)

#include "core/message_macros/message_data_definition_macros.h"
#include "core/node_message_defs.h"

#include "core/message_macros/undef_message_macros.h"

#include "core/message_macros/message_handle_data_definition_macros.h"
#include "core/node_message_defs.h"

#include "core/message_macros/undef_message_macros.h"

#include "core/message_macros/message_handle_definition_macros.h"
#include "core/node_message_defs.h"

#include "core/message_macros/undef_message_macros.h"

#include "core/message_macros/message_definition_macros.h"
#include "core/node_message_defs.h"

#include "core/message_macros/undef_message_macros.h"

#pragma pack(pop)

}  // namespace msg
}  // namespace core
}  // namespace ipcz
