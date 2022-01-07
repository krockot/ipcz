// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CORE_NODE_MESSAGES_H_
#define CORE_NODE_MESSAGES_H_

#include <cstdint>

#include "core/buffer_id.h"
#include "core/driver_transport.h"
#include "core/message_internal.h"
#include "core/node_link_address.h"
#include "core/node_name.h"
#include "core/router_descriptor.h"
#include "core/routing_id.h"
#include "core/sequence_number.h"
#include "ipcz/ipcz.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {
namespace msg {

// This file is used to push message definitions through the preprocessor to
// emit message structure declarations. See node_message_defs.h for the actual
// message definitions.

#pragma pack(push, 1)

// clang-format off
#include "core/message_macros/message_params_declaration_macros.h"
#include "core/node_message_defs.h"
#include "core/message_macros/undef_message_macros.h"

#include "core/message_macros/message_declaration_macros.h"
#include "core/node_message_defs.h"
#include "core/message_macros/undef_message_macros.h"
// clang-format on

#pragma pack(pop)

// TODO: Messages defined here are dynamically size due to variable-length
// array fields. Support dynamic message sizing and array-typed fields with the
// cheesy message macro scheme so that we can avoid the one-offs.

// Requests that a broker node accept a new non-broker client, introduced
// indirectly by some established non-broker client on the new client's behalf.
// This message supports ConnectNode() calls which specify
// IPCZ_CONNECT_NODE_SHARE_BROKER. The calling node in that case sends this
// message -- which also contains a serialized representation of the transport
// given to the call -- to its broker.
//
// The broker then uses the transport to complete a special handshake with the
// new client node (via ConnectFromBrokerIndirect and ConnectToBrokerIndirect),
// and it responds to the sender of this message with an
// AcceptIndirectBrokerConnection.
//
// Finally the broker then introduces the sender of this message to the new
// client using the usual IntroduceNode messages. Each non-broker node by that
// point has enough information (by receiving either ConnectFromBrokerIndirect
// or AcceptIndirectBrokerConnection) to expect that introduction and use it to
// establish initial portals between the two non-broker nodes as their original
// ConnectNode() calls intended.
struct IPCZ_ALIGN(16) RequestIndirectBrokerConnection {
  static constexpr uint8_t kId = 10;
  internal::MessageHeader message_header;
  uint64_t request_id;
  uint32_t num_initial_portals;
  uint32_t num_transport_bytes;
  uint32_t num_transport_os_handles;
};

}  // namespace msg
}  // namespace core
}  // namespace ipcz

#endif  // CORE_NODE_MESSAGES_H_
