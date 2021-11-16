// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/transport.h"

#include <utility>

#include "core/node.h"
#include "core/node_link.h"
#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "util/handle_util.h"

namespace ipcz {
namespace core {

namespace {

IpczResult NotifyTransport(IpczHandle transport,
                           const uint8_t* data,
                           uint32_t num_bytes,
                           const struct IpczOSHandle* os_handles,
                           uint32_t num_os_handles,
                           IpczTransportActivityFlags flags,
                           const void* options) {
  if (transport == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (flags & IPCZ_TRANSPORT_ACTIVITY_DEACTIVATED) {
    mem::Ref<Transport> doomed_transport(mem::RefCounted::kAdoptExistingRef,
                                         ToPtr<Transport>(transport));
    return IPCZ_RESULT_OK;
  }

  Transport& t = ToRef<Transport>(transport);
  if (flags & IPCZ_TRANSPORT_ACTIVITY_ERROR) {
    t.NotifyError();
    return IPCZ_RESULT_OK;
  }

  absl::InlinedVector<os::Handle, 2> handles;
  handles.resize(num_os_handles);
  for (size_t i = 0; i < num_os_handles; ++i) {
    handles[i] = os::Handle::FromIpczOSHandle(os_handles[i]);
  }

  t.Notify(Transport::Message(absl::Span<const uint8_t>(data, num_bytes),
                              absl::MakeSpan(handles)));
  return IPCZ_RESULT_OK;
}

}  // namespace

Transport::Descriptor::Descriptor() = default;

Transport::Descriptor::Descriptor(Descriptor&&) = default;

Transport::Descriptor& Transport::Descriptor::operator=(Descriptor&&) = default;

Transport::Descriptor::~Descriptor() = default;

Transport::Data::Data() = default;

Transport::Data::Data(absl::Span<const uint8_t> data) : Span(data) {}

Transport::Data::Data(absl::Span<const char> str)
    : Span(reinterpret_cast<const uint8_t*>(str.data()), str.size()) {}

absl::Span<const char> Transport::Data::AsString() const {
  return absl::MakeSpan(reinterpret_cast<const char*>(data()), size());
}

Transport::Message::Message(Data data) : data(data) {}

Transport::Message::Message(Data data, absl::Span<os::Handle> handles)
    : data(data), handles(handles) {}

Transport::Message::Message(const Message&) = default;

Transport::Message& Transport::Message::operator=(const Message&) = default;

Transport::Message::~Message() = default;

Transport::Transport(mem::Ref<Node> node, IpczDriverHandle driver_transport)
    : node_(std::move(node)),
      driver_(node_->driver()),
      driver_transport_(driver_transport) {}

Transport::~Transport() = default;

IpczResult Transport::Activate(mem::Ref<NodeLink> for_link) {
  node_link_ = std::move(for_link);

  // Acquire a self-reference, balanced in NotifyTransport() when the driver
  // invokes its activity handler with IPCZ_TRANSPORT_ACTIVITY_DEACTIVATED.
  IpczHandle handle = ToHandle(mem::WrapRefCounted(this).release());
  return driver_.ActivateTransport(driver_transport_, handle, NotifyTransport,
                                   IPCZ_NO_FLAGS, nullptr);
}

IpczResult Transport::Deactivate() {
  return driver_.DestroyTransport(driver_transport_, IPCZ_NO_FLAGS, nullptr);
}

IpczResult Transport::Transmit(const Message& message) {
  absl::InlinedVector<IpczOSHandle, 2> os_handles(message.handles.size());
  for (size_t i = 0; i < message.handles.size(); ++i) {
    os_handles[i].size = sizeof(os_handles[i]);
    bool ok = os::Handle::ToIpczOSHandle(std::move(message.handles[i]),
                                         &os_handles[i]);
    ABSL_ASSERT(ok);
  }

  return driver_.Transmit(
      driver_transport_, message.data.data(),
      static_cast<uint32_t>(message.data.size()), os_handles.data(),
      static_cast<uint32_t>(os_handles.size()), IPCZ_NO_FLAGS, nullptr);
}

void Transport::Notify(const Message& message) {
  ABSL_ASSERT(node_link_);
  node_link_->OnMessageFromTransport(message);
}

void Transport::NotifyError() {
  ABSL_ASSERT(node_link_);
  node_link_->OnTransportError();
}

}  // namespace core
}  // namespace ipcz
