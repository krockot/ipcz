// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node_link.h"

#include "core/message_internal.h"
#include "core/node.h"
#include "core/node_messages.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace core {

NodeLink::NodeLink(Node& node, os::Channel channel, os::Process remote_process)
    : node_(mem::WrapRefCounted(&node)),
      channel_(std::move(channel)),
      remote_process_(std::move(remote_process)) {
  channel_.Listen([this](os::Channel::Message message) {
    return this->OnMessage(message);
  });
}

NodeLink::~NodeLink() = default;

void NodeLink::Send(absl::Span<uint8_t> data, absl::Span<os::Handle> handles) {
  ABSL_ASSERT(channel_.is_valid());
  ABSL_ASSERT(data.size() >= sizeof(internal::MessageHeader));
  internal::MessageHeader& header =
      *reinterpret_cast<internal::MessageHeader*>(data.data());
  header.request_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
  channel_.Send(os::Channel::Data(data));
}

bool NodeLink::OnMessage(os::Channel::Message message) {
  if (message.data.size() < sizeof(internal::MessageHeader)) {
    return false;
  }

  const auto& header =
      *reinterpret_cast<const internal::MessageHeader*>(message.data.data());
  if (header.size < sizeof(internal::MessageHeader)) {
    return false;
  }

  // switch (header.message_id) {

  // }
  return true;
}

}  // namespace core
}  // namespace ipcz
