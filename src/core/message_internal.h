// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_MESSAGE_INTERNAL_H_
#define IPCZ_SRC_CORE_MESSAGE_INTERNAL_H_

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "ipcz/ipcz.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/types/span.h"

#pragma pack(push, 1)

namespace ipcz {
namespace core {
namespace internal {

// Header which begins all messages. The header layout is versioned for
// extensibility and long-term support.
struct IPCZ_ALIGN(8) MessageHeader {
  // The size of the header in bytes.
  uint8_t size;

  // The header version in use by this message.
  uint8_t version;

  // Message ID assigned as part of a message's type definition via MSG_START().
  // Note that for messages with replies, the same message ID is used for both
  // the request and the reply message.
  uint8_t message_id;

  // Indicates that this is a request message which expects a reply. A
  // well-behaved node must send a message with the same `message_id` and
  // `request_id` and either the `is_reply` bit set (along with an appropriate
  // message payload) or the `wont_reply` bit set, indicating that it will never
  // reply to that message.
  bool expects_reply : 1;

  // Indicates that this message is a reply to a previous request with the same
  // `message_id` and `request_id`.
  bool is_reply : 1;

  // Indicates that a request with the same `message_id` and `request_id` has
  // been received, but that the node will not be respond to it, for example
  // because they don't understand the message or don't know how to respond to
  // it.
  bool wont_reply : 1;

  // A semi-unique integer which is incremented for every round-trip message
  // (messages which expect a reply or which are a reply) sent on a single
  // NodeLink. Semi-unique because this can overflow and wrap around, but a
  // NodeLink will not reuse a request ID whose last use has yet to be
  // acknowledged.
  //
  // This value is ignored on messages which don't set one of the
  // `expects_reply`, `is_reply`, or `wont_reply` flags.
  uint32_t request_id;
};
static_assert(sizeof(MessageHeader) == 8, "Unexpected size");

struct IPCZ_ALIGN(8) StructHeader {
  uint32_t size;
  uint32_t version;
};
static_assert(sizeof(StructHeader) == 8, "Unexpected size");

}  // namespace internal
}  // namespace core
}  // namespace ipcz

#pragma pack(pop)

#endif  // IPCZ_SRC_CORE_MESSAGE_INTERNAL_H_
