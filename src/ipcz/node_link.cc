// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/node_link.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "ipcz/box.h"
#include "ipcz/fragment.h"
#include "ipcz/fragment_descriptor.h"
#include "ipcz/handle_descriptor.h"
#include "ipcz/ipcz.h"
#include "ipcz/message_internal.h"
#include "ipcz/node.h"
#include "ipcz/node_messages.h"
#include "ipcz/portal.h"
#include "ipcz/remote_router_link.h"
#include "ipcz/router.h"
#include "ipcz/router_descriptor.h"
#include "ipcz/router_link.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "util/handle_util.h"
#include "util/log.h"
#include "util/ref_counted.h"

#if defined(THREAD_SANITIZER)
#include <sanitizer/tsan_interface.h>
#endif

namespace ipcz {

// static
Ref<NodeLink> NodeLink::Create(Ref<Node> node,
                               LinkSide link_side,
                               const NodeName& local_node_name,
                               const NodeName& remote_node_name,
                               Node::Type remote_node_type,
                               uint32_t remote_protocol_version,
                               Ref<DriverTransport> transport,
                               Ref<NodeLinkMemory> memory) {
  auto link = WrapRefCounted(
      new NodeLink(std::move(node), link_side, local_node_name,
                   remote_node_name, remote_node_type, remote_protocol_version,
                   std::move(transport), std::move(memory)));
  link->memory().SetNodeLink(link);
  return link;
}

NodeLink::NodeLink(Ref<Node> node,
                   LinkSide link_side,
                   const NodeName& local_node_name,
                   const NodeName& remote_node_name,
                   Node::Type remote_node_type,
                   uint32_t remote_protocol_version,
                   Ref<DriverTransport> transport,
                   Ref<NodeLinkMemory> memory)
    : node_(std::move(node)),
      link_side_(link_side),
      local_node_name_(local_node_name),
      remote_node_name_(remote_node_name),
      remote_node_type_(remote_node_type),
      remote_protocol_version_(remote_protocol_version),
      transport_(std::move(transport)),
      memory_(std::move(memory)) {
  transport_->set_listener(this);
}

NodeLink::~NodeLink() {
  Deactivate();
}

Ref<RemoteRouterLink> NodeLink::AddRemoteRouterLink(
    SublinkId sublink,
    FragmentRef<RouterLinkState> link_state,
    LinkType type,
    LinkSide side,
    Ref<Router> router) {
  auto link = RemoteRouterLink::Create(WrapRefCounted(this), sublink,
                                       std::move(link_state), type, side);

  absl::MutexLock lock(&mutex_);
  auto [it, added] = sublinks_.try_emplace(
      sublink, Sublink(std::move(link), std::move(router)));
  if (!added) {
    return nullptr;
  }
  return it->second.router_link;
}

bool NodeLink::RemoveRemoteRouterLink(SublinkId sublink) {
  absl::MutexLock lock(&mutex_);
  auto it = sublinks_.find(sublink);
  if (it == sublinks_.end()) {
    return false;
  }

  sublinks_.erase(sublink);
  return true;
}

absl::optional<NodeLink::Sublink> NodeLink::GetSublink(SublinkId sublink) {
  absl::MutexLock lock(&mutex_);
  auto it = sublinks_.find(sublink);
  if (it == sublinks_.end()) {
    return absl::nullopt;
  }
  return it->second;
}

Ref<Router> NodeLink::GetRouter(SublinkId sublink) {
  absl::MutexLock lock(&mutex_);
  auto it = sublinks_.find(sublink);
  if (it == sublinks_.end()) {
    return nullptr;
  }
  return it->second.receiver;
}

void NodeLink::Deactivate() {
  SublinkMap sublinks;
  {
    absl::MutexLock lock(&mutex_);
    sublinks = std::move(sublinks_);
    if (!active_) {
      return;
    }

    active_ = false;
  }

  memory_->SetNodeLink(nullptr);
  sublinks.clear();
  transport_->Deactivate();
}

void NodeLink::RequestIndirectBrokerConnection(
    Ref<DriverTransport> transport,
    size_t num_initial_portals,
    IndirectBrokerConnectionCallback callback) {
  uint64_t request_id;
  {
    absl::MutexLock lock(&mutex_);
    request_id = next_request_id_++;
    pending_indirect_broker_connections_[request_id] = std::move(callback);
  }

  msg::RequestIndirectBrokerConnection request;
  request.params().request_id = request_id;
  request.params().num_initial_portals =
      static_cast<uint32_t>(num_initial_portals);
  request.AppendDriverObject(transport->TakeDriverObject(),
                             request.params().transport);
  Transmit(request);
}

void NodeLink::RequestIntroduction(const NodeName& name) {
  msg::RequestIntroduction request;
  request.params().name = name;
  Transmit(request);
}

void NodeLink::IntroduceNode(const NodeName& name,
                             LinkSide link_side,
                             Ref<DriverTransport> transport,
                             DriverMemory link_buffer_memory) {
  msg::IntroduceNode intro;
  intro.params().name = name;
  intro.params().known = (transport != nullptr);
  intro.params().link_side = link_side;
  intro.AppendDriverObject(transport->TakeDriverObject(),
                           intro.params().transport);
  intro.AppendDriverObject(link_buffer_memory.TakeDriverObject(),
                           intro.params().buffer);
  Transmit(intro);
}

bool NodeLink::BypassProxy(const NodeName& proxy_name,
                           SublinkId proxy_sublink,
                           SequenceNumber proxy_outbound_sequence_length,
                           Ref<Router> new_peer) {
  // Note that by convention the side which initiates a bypass (this side)
  // adopts side A of the new bypass link. The other end adopts side B.
  const SublinkId new_sublink = memory().AllocateSublinkIds(1);
  FragmentRef<RouterLinkState> state = memory().AllocateRouterLinkState();
  Ref<RouterLink> new_link = AddRemoteRouterLink(
      new_sublink, state, LinkType::kCentral, LinkSide::kA, new_peer);

  DVLOG(4) << "Sending BypassProxy from " << local_node_name_.ToString()
           << " to " << remote_node_name_.ToString() << " with new sublink "
           << new_sublink << " to replace its link to proxy "
           << proxy_name.ToString() << " on sublink " << proxy_sublink;

  msg::BypassProxy bypass;
  bypass.params().proxy_name = proxy_name;
  bypass.params().proxy_sublink = proxy_sublink;
  bypass.params().new_sublink = new_sublink;
  bypass.params().new_link_state_fragment = state.release().descriptor();
  bypass.params().proxy_outbound_sequence_length =
      proxy_outbound_sequence_length;
  Transmit(bypass);

  // This link is only provided after we transmit the bypass request, ensuring
  // that `new_peer` doesn't send anything else over the link until the bypass
  // has been accepted by the remote node.
  new_peer->SetOutwardLink(new_link);

  return true;
}

void NodeLink::AddFragmentAllocatorBuffer(BufferId buffer_id,
                                          uint32_t fragment_size,
                                          DriverMemory memory) {
  msg::AddFragmentAllocatorBuffer add;
  add.params().buffer_id = buffer_id;
  add.params().fragment_size = fragment_size;
  add.AppendDriverObject(memory.TakeDriverObject(), add.params().buffer);
  Transmit(add);
}

void NodeLink::RequestMemory(uint32_t size, RequestMemoryCallback callback) {
  {
    absl::MutexLock lock(&mutex_);
    pending_memory_requests_[size].push_back(std::move(callback));
  }

  msg::RequestMemory request;
  request.params().size = size;
  Transmit(request);
}

bool NodeLink::DispatchRelayedMessage(msg::AcceptRelayedMessage& relay) {
  // TODO: clean up these hacks

  absl::Span<uint8_t> data = relay.GetArrayView<uint8_t>(relay.params().data);
  if (data.size() < sizeof(internal::MessageHeaderV0)) {
    return false;
  }

  internal::MessageHeaderV0& header = internal::GetMessageHeader(data);
  if (!internal::IsMessageHeaderValid(data)) {
    return false;
  }

  switch (header.message_id) {
    case msg::AcceptParcel::kId: {
      msg::AcceptParcel accept;
      accept.Adopt(data, relay.driver_objects());
      return OnAcceptParcel(accept);
    }

    case msg::AcceptParcelDriverObjects::kId: {
      msg::AcceptParcelDriverObjects accept;
      accept.Adopt(data, relay.driver_objects());
      return OnAcceptParcelDriverObjects(accept);
    }

    case msg::AddFragmentAllocatorBuffer::kId: {
      msg::AddFragmentAllocatorBuffer add;
      add.Adopt(data, relay.driver_objects());
      return OnAddFragmentAllocatorBuffer(add);
    }

    default:
      DVLOG(2) << "Ignoring unexpected broker message relay for ID "
               << static_cast<int>(header.message_id);
      return true;
  }
}

void NodeLink::DiagnoseForTesting() {
  LOG(ERROR) << "Link from " << local_node_name_.ToString() << " to "
             << remote_node_name_.ToString();

  // Add other diagnostic logging here as needed.
}

void NodeLink::TransmitMessage(
    internal::MessageBase& message,
    absl::Span<const internal::ParamMetadata> metadata) {
  if (!message.CanTransmitOn(*transport_)) {
    // The driver has indicated that it can't transmit this message through our
    // transport, so the message must instead be relayed through a broker.
    auto broker = node_->GetBrokerLink();
    if (!broker) {
      DLOG(ERROR) << "Cannot relay message without a broker link.";
      return;
    }

    broker->RelayMessage(remote_node_name_, message);
    return;
  }

  if (!message.Serialize(metadata, *transport_)) {
    DLOG(ERROR) << "Failed to serialize message";
    return;
  }

  // Note that we don't allocate a SequenceNumber until after succeeding or
  // failing at fragment allocation below, because a successfully allocated
  // fragment may reside in a buffer which is just about to be shared with the
  // remote node. If the SequenceNumber of that fragment's message gets
  // sequenced before that of the message which shares the buffer, the
  // recipient's message queue will be permanently stuck.
  absl::optional<SequenceNumber> sequence_number;

  // For small messages we prefer transmission through shared memory blocks, and
  // we prefer to allocate them as one of the few size classes with a fixed
  // allocator in the primary buffer. This means the primary buffer ends up
  // being the most common message allocation source.
  size_t fragment_size = 0;
  if (message.data_view().size() <= 256) {
    fragment_size = 256;
  } else if (message.data_view().size() <= 512) {
    fragment_size = 512;
  } else if (message.data_view().size() <= 1024) {
    fragment_size = 1024;
  } else if (message.data_view().size() <= 2048) {
    fragment_size = 2048;
  } else {
    fragment_size = message.data_view().size();
  }
  constexpr size_t kMaxSizeFallbackScale = 6;
  while (message.transmissible_driver_handles().empty()) {
    Fragment fragment = memory().fragment_allocator().Allocate(fragment_size);
    if (fragment.is_null()) {
      if (fragment_size > 2048 ||
          fragment_size > message.data_view().size() * kMaxSizeFallbackScale) {
        break;
      }

      fragment_size *= 2;
      continue;
    }

    sequence_number =
        next_outgoing_sequence_number_.fetch_add(1, std::memory_order_relaxed);
    message.header().sequence_number = *sequence_number;
    memcpy(&fragment.mutable_bytes()[sizeof(uint32_t)],
           &message.data_view().data()[sizeof(uint32_t)],
           message.data_view().size() - sizeof(uint32_t));
    std::atomic_store_explicit(
        static_cast<std::atomic<uint32_t>*>(fragment.address()),
        *reinterpret_cast<uint32_t*>(message.data_view().data()),
        std::memory_order_release);

    NodeLinkMemory::MessageFragment message_fragment(*sequence_number,
                                                     fragment.descriptor());
    if (!memory().outgoing_message_fragments().Push(message_fragment)) {
      // No queue capacity at the moment...
      memory().fragment_allocator().Free(fragment);
      break;
    }

    if (memory().TestAndSetNotificationPending()) {
      // No flush necessary as the remote node already has some pending I/O to
      // wake it up.
      return;
    }

    msg::FlushLink flush;
    flush.header().sequence_number =
        next_outgoing_sequence_number_.fetch_add(1, std::memory_order_relaxed);
    transport_->TransmitMessage(
        DriverTransport::Message(DriverTransport::Data(flush.data_view())));
    return;
  }

  if (!sequence_number) {
    sequence_number =
        next_outgoing_sequence_number_.fetch_add(1, std::memory_order_relaxed);
  }

  memory().TestAndSetNotificationPending();
  message.header().sequence_number = *sequence_number;
  transport_->TransmitMessage(
      DriverTransport::Message(DriverTransport::Data(message.data_view()),
                               message.transmissible_driver_handles()));
}

void NodeLink::RelayMessage(const NodeName& to_node,
                            internal::MessageBase& message) {
  ABSL_ASSERT(remote_node_type_ == Node::Type::kBroker);
  msg::RelayMessage relay;
  relay.params().destination = to_node;
  relay.params().data =
      relay.AllocateArray<uint8_t>(message.data_view().size());
  memcpy(relay.GetArrayData(relay.params().data), message.data_view().data(),
         message.data_view().size());
  relay.params().driver_objects =
      relay.AppendDriverObjects(message.driver_objects());
  Transmit(relay);
}

IpczResult NodeLink::OnTransportMessage(
    const DriverTransport::Message& message) {
  IpczResult result = FlushIncomingMessages();
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  result = DispatchOrQueueTransportMessage(message);
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  // Note that we don't clear this flag until *after* the above flush. This
  // allows slightly more time for the remote node to transmit additional
  // messages in shared memory without signaling us, but it also means we need
  // to do another flush before returning.
  memory().ClearPendingNotification();

  return FlushIncomingMessages();
}

void NodeLink::OnTransportError() {
  SublinkMap sublinks;
  {
    absl::MutexLock lock(&mutex_);
    broken_ = true;
    std::swap(sublinks, sublinks_);
  }

  for (auto& [id, sublink] : sublinks) {
    sublink.receiver->NotifyLinkDisconnected(*this, id);
  }
}

IpczResult NodeLink::DispatchOrQueueTransportMessage(
    const DriverTransport::Message& message) {
  const auto& header =
      *reinterpret_cast<const internal::MessageHeader*>(message.data.data());
  const SequenceNumber sequence_number = header.sequence_number;
  {
    absl::MutexLock lock(&mutex_);
    if (sequence_number < incoming_messages_.current_sequence_number()) {
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    if (sequence_number > incoming_messages_.current_sequence_number() ||
        is_processing_incoming_messages_) {
      IncomingMessage incoming_message;
      incoming_message.data =
          std::vector<uint8_t>(message.data.begin(), message.data.end());
      incoming_message.handles = std::vector<IpczDriverHandle>(
          message.handles.begin(), message.handles.end());
      bool ok =
          incoming_messages_.Push(sequence_number, std::move(incoming_message));
      ABSL_ASSERT(ok);
      return IPCZ_RESULT_OK;
    }

    // Otherwise this message is the next to dispatch, so fall through.
    is_processing_incoming_messages_ = true;
    incoming_messages_.SkipNextSequenceNumber();
  }

  IpczResult result = DispatchMessage(message);
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  {
    absl::MutexLock lock(&mutex_);
    is_processing_incoming_messages_ = false;
  }
  return FlushIncomingMessages();
}

IpczResult NodeLink::DispatchOrQueueFragmentMessage(
    SequenceNumber sequence_number,
    const FragmentDescriptor& descriptor) {
  const Fragment fragment = memory().GetFragment(descriptor);
  {
    absl::MutexLock lock(&mutex_);
    if (sequence_number < incoming_messages_.current_sequence_number()) {
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    if (sequence_number > incoming_messages_.current_sequence_number() ||
        !fragment.is_resolved()) {
      IncomingMessage incoming_message;
      incoming_message.fragment = fragment;
      bool ok =
          incoming_messages_.Push(sequence_number, std::move(incoming_message));
      ABSL_ASSERT(ok);
      return IPCZ_RESULT_OK;
    }

    // Otherwise this message is the next to dispatch, so fall through.
    incoming_messages_.SkipNextSequenceNumber();
  }

#if defined(THREAD_SANITIZER)
  // Annotate for TSAN that subsequent access to the fragment's contents is
  // properly synchronized, since it can't deduce this on its own.
  // Synchronization occurs by virtue of push and pop operations on the
  // MessageFragment queue which transported this fragment descriptor.
  __tsan_acquire(fragment.address());
#endif

  IpczResult result =
      DispatchMessage(DriverTransport::Message(fragment.bytes()));
  memory().fragment_allocator().Free(fragment);
  return result;
}

IpczResult NodeLink::FlushIncomingMessages() {
  {
    absl::MutexLock lock(&mutex_);
    if (is_processing_incoming_messages_ || is_next_incoming_message_blocked_) {
      return IPCZ_RESULT_OK;
    }

    // NOTE: We don't bother to unset this in any failure return paths here,
    // since failure implies we don't want to read from the link anymore anyway.
    is_processing_incoming_messages_ = true;
  }

  MpscQueue<NodeLinkMemory::MessageFragment>& fragments =
      memory().incoming_message_fragments();
  for (;;) {
    while (const auto* const message_fragment = fragments.Peek()) {
      const SequenceNumber sequence_number = message_fragment->sequence_number;
      const FragmentDescriptor descriptor = message_fragment->descriptor;
      fragments.Pop();
      IpczResult result =
          DispatchOrQueueFragmentMessage(sequence_number, descriptor);
      if (result != IPCZ_RESULT_OK) {
        return result;
      }
    }

    absl::optional<BufferId> wait_for_buffer;
    IncomingMessage next_message;
    {
      absl::MutexLock lock(&mutex_);
      if (!incoming_messages_.HasNextElement()) {
        if (fragments.Peek()) {
          // No more messages in this queue, but the shared memory queue has
          // more messages now.
          continue;
        }

        is_processing_incoming_messages_ = false;
        return IPCZ_RESULT_OK;
      }

      IncomingMessage& front = incoming_messages_.NextElement();
      if (front.fragment.is_pending()) {
        front.fragment = memory().GetFragment(front.fragment.descriptor());
      }

      if (front.fragment.is_pending()) {
        // Still pending, so we have to wait for its buffer before we can kee
        // processing incoming messages.
        wait_for_buffer = front.fragment.buffer_id();
        is_next_incoming_message_blocked_ = true;
        is_processing_incoming_messages_ = false;
      } else {
        incoming_messages_.Pop(next_message);
      }
    }

    if (wait_for_buffer) {
      // This may invoke the callback synchronously if the buffer is already
      // available since we last tried resolving the fragment, so we don't want
      // to hold `mutex_` while calling this.
      memory().OnBufferAvailable(
          *wait_for_buffer, [self = WrapRefCounted(this)] {
            {
              absl::MutexLock lock(&self->mutex_);
              self->is_next_incoming_message_blocked_ = false;
            }
            self->FlushIncomingMessages();
          });
      return IPCZ_RESULT_OK;
    }

    IpczResult result;
    if (!next_message.fragment.is_null()) {
#if defined(THREAD_SANITIZER)
      // Annotate for TSAN that subsequent access to the fragment's contents is
      // properly synchronized, since it can't deduce this on its own.
      // Synchronization occurs by virtue of push and pop operations on the
      // MessageFragment queue which transported this fragment descriptor.
      __tsan_acquire(next_message.fragment.address());
#endif

      result = DispatchMessage(DriverTransport::Message(
          DriverTransport::Data(next_message.fragment.bytes())));
      memory().fragment_allocator().Free(next_message.fragment);
    } else {
      result = DispatchMessage(DriverTransport::Message(
          DriverTransport::Data(absl::MakeSpan(next_message.data)),
          absl::MakeSpan(next_message.handles)));
    }
    if (result != IPCZ_RESULT_OK) {
      return result;
    }
  }
}

bool NodeLink::OnConnectFromBrokerToNonBroker(
    msg::ConnectFromBrokerToNonBroker& connect) {
  // Only accepted early in a transport's lifetime, before any NodeLink is
  // listening. See NodeConnector.
  return false;
}

bool NodeLink::OnConnectFromNonBrokerToBroker(
    msg::ConnectFromNonBrokerToBroker& connect) {
  // Only accepted early in a transport's lifetime, before any NodeLink is
  // listening. See NodeConnector.
  return false;
}

bool NodeLink::OnConnectToBrokerIndirect(
    msg::ConnectToBrokerIndirect& connect) {
  // Only accepted early in a transport's lifetime, before any NodeLink is
  // listening. See NodeConnector.
  return false;
}

bool NodeLink::OnConnectFromBrokerIndirect(
    msg::ConnectFromBrokerIndirect& connect) {
  // Only accepted early in a transport's lifetime, before any NodeLink is
  // listening. See NodeConnector.
  return false;
}

bool NodeLink::OnConnectFromBrokerToBroker(
    msg::ConnectFromBrokerToBroker& connect) {
  // Only accepted early in a transport's lifetime, before any NodeLink is
  // listening. See NodeConnector.
  return false;
}

bool NodeLink::OnRequestIndirectBrokerConnection(
    msg::RequestIndirectBrokerConnection& request) {
  if (node_->type() != Node::Type::kBroker) {
    return false;
  }

  auto transport = request.TakeDriverObject(request.params().transport);
  if (!transport.is_valid()) {
    return false;
  }
  return node_->OnRequestIndirectBrokerConnection(
      *this, request.params().request_id,
      MakeRefCounted<DriverTransport>(std::move(transport)),
      request.params().num_initial_portals);
}

bool NodeLink::OnAcceptIndirectBrokerConnection(
    const msg::AcceptIndirectBrokerConnection& accept) {
  if (node_->type() == Node::Type::kBroker ||
      remote_node_type_ != Node::Type::kBroker) {
    return false;
  }

  IndirectBrokerConnectionCallback callback;
  {
    absl::MutexLock lock(&mutex_);
    auto it =
        pending_indirect_broker_connections_.find(accept.params().request_id);
    if (it == pending_indirect_broker_connections_.end()) {
      return false;
    }

    callback = std::move(it->second);
    pending_indirect_broker_connections_.erase(it);
  }

  if (!accept.params().success) {
    callback(NodeName(), 0);
  } else {
    callback(accept.params().connected_node_name,
             accept.params().num_remote_portals);
  }
  return true;
}

bool NodeLink::OnAcceptParcel(msg::AcceptParcel& accept) {
  absl::Span<const uint8_t> parcel_data =
      accept.GetArrayView<uint8_t>(accept.params().parcel_data);
  absl::Span<const HandleDescriptor> handle_descriptors =
      accept.GetArrayView<HandleDescriptor>(accept.params().handle_descriptors);
  absl::Span<const RouterDescriptor> new_routers =
      accept.GetArrayView<RouterDescriptor>(accept.params().new_routers);
  auto driver_objects = accept.driver_objects();

  bool is_split_parcel = false;
  Parcel::ObjectVector objects(handle_descriptors.size());
  for (size_t i = 0; i < handle_descriptors.size(); ++i) {
    const HandleDescriptor& descriptor = handle_descriptors[i];
    switch (descriptor.type) {
      case HandleDescriptor::kPortal: {
        if (new_routers.empty()) {
          return false;
        }

        Ref<Router> new_router = Router::Deserialize(new_routers[0], *this);
        if (!new_router) {
          return false;
        }

        objects[i] = MakeRefCounted<Portal>(node_, std::move(new_router));
        new_routers.remove_prefix(1);
        break;
      }

      case HandleDescriptor::kBox: {
        if (driver_objects.empty()) {
          return false;
        }

        objects[i] = MakeRefCounted<Box>(std::move(driver_objects[0]));
        driver_objects.remove_prefix(1);
        break;
      }

      case HandleDescriptor::kBoxRelayed: {
        is_split_parcel = true;
        break;
      }

      default:
        return false;
    }
  }

  const SublinkId for_sublink = accept.params().sublink;
  Parcel parcel(accept.params().sequence_number);
  parcel.SetObjects(std::move(objects));
  if (!accept.params().parcel_fragment.is_null()) {
    if (accept.params().parcel_size > accept.params().parcel_fragment.size()) {
      return false;
    }

    const FragmentDescriptor descriptor = accept.params().parcel_fragment;
    const Fragment fragment = memory_->GetFragment(descriptor);
    parcel.SetDataFragment(memory_, fragment);
    if (fragment.is_pending()) {
      memory_->OnBufferAvailable(
          descriptor.buffer_id(),
          [self = WrapRefCounted(this), for_sublink, parcel = std::move(parcel),
           is_split_parcel, size = accept.params().parcel_size]() mutable {
            if (parcel.ResolveDataFragment()) {
              parcel.SetDataSize(size);
              if (is_split_parcel) {
                self->AcceptParcelWithoutDriverObjects(for_sublink, parcel);
              }
              self->AcceptCompleteParcel(for_sublink, parcel);
            }
          });
      return true;
    } else {
      parcel.SetDataSize(accept.params().parcel_size);
    }
  } else {
    parcel.SetInlinedData(
        std::vector<uint8_t>(parcel_data.begin(), parcel_data.end()));
  }

  if (is_split_parcel) {
    return AcceptParcelWithoutDriverObjects(for_sublink, parcel);
  }
  return AcceptCompleteParcel(for_sublink, parcel);
}

bool NodeLink::OnAcceptParcelDriverObjects(
    msg::AcceptParcelDriverObjects& accept) {
  Parcel parcel(accept.params().sequence_number);
  Parcel::ObjectVector objects;
  objects.reserve(accept.driver_objects().size());
  for (auto& object : accept.driver_objects()) {
    objects.push_back(MakeRefCounted<Box>(std::move(object)));
  }
  parcel.SetObjects(std::move(objects));
  return AcceptParcelDriverObjects(accept.params().sublink, parcel);
}

bool NodeLink::OnRouteClosed(const msg::RouteClosed& route_closed) {
  absl::optional<Sublink> sublink = GetSublink(route_closed.params().sublink);
  if (!sublink) {
    return true;
  }

  return sublink->receiver->AcceptRouteClosureFrom(
      sublink->router_link->GetType(), route_closed.params().sequence_length);
}

bool NodeLink::OnSetRouterLinkStateFragment(
    const msg::SetRouterLinkStateFragment& set) {
  absl::optional<Sublink> sublink = GetSublink(set.params().sublink);
  if (!sublink) {
    return true;
  }

  sublink->router_link->SetLinkState(
      memory().AdoptFragmentRef<RouterLinkState>(set.params().descriptor));
  return true;
}

bool NodeLink::OnRouteDisconnected(const msg::RouteDisconnected& disconnect) {
  const SublinkId sublink = disconnect.params().sublink;
  Ref<Router> router = GetRouter(sublink);
  if (!router) {
    return true;
  }

  router->NotifyLinkDisconnected(*this, sublink);
  return true;
}

bool NodeLink::OnNotifyDataConsumed(const msg::NotifyDataConsumed& notify) {
  Ref<Router> router = GetRouter(notify.params().sublink);
  if (!router) {
    return true;
  }

  router->NotifyOutwardPeerConsumedData();
  return true;
}

bool NodeLink::OnRequestIntroduction(const msg::RequestIntroduction& request) {
  return node_->OnRequestIntroduction(*this, request);
}

bool NodeLink::OnIntroduceNode(msg::IntroduceNode& intro) {
  if (remote_node_type_ != Node::Type::kBroker) {
    return false;
  }

  const NodeName name = intro.params().name;
  const bool known = intro.params().known;
  if (!known) {
    node_->OnIntroduceNode(name, known, intro.params().link_side, nullptr,
                           nullptr);
    return true;
  }

  DriverObject transport_object =
      intro.TakeDriverObject(intro.params().transport);
  DriverMemory memory(intro.TakeDriverObject(intro.params().buffer));
  if (!transport_object.is_valid() || !memory.is_valid()) {
    return false;
  }
  return node_->OnIntroduceNode(
      name, known, intro.params().link_side,
      NodeLinkMemory::Adopt(node_, std::move(memory)),
      MakeRefCounted<DriverTransport>(std::move(transport_object)));
}

bool NodeLink::OnAddFragmentAllocatorBuffer(
    msg::AddFragmentAllocatorBuffer& add) {
  DriverMemory buffer_memory(add.TakeDriverObject(add.params().buffer));
  if (!buffer_memory.is_valid()) {
    return false;
  }

  if (!memory().AddFragmentAllocatorBuffer(add.params().buffer_id,
                                           add.params().fragment_size,
                                           std::move(buffer_memory))) {
    return false;
  }

  return true;
}

bool NodeLink::OnStopProxying(const msg::StopProxying& stop) {
  Ref<Router> router = GetRouter(stop.params().sublink);
  if (!router) {
    DVLOG(4) << "Received StopProxying for unknown router";
    return true;
  }

  DVLOG(4) << "Received StopProxying on " << local_node_name_.ToString()
           << " sublink " << stop.params().sublink << " with inbound"
           << " length " << stop.params().proxy_inbound_sequence_length
           << " and outbound length "
           << stop.params().proxy_outbound_sequence_length;

  return router->StopProxying(stop.params().proxy_inbound_sequence_length,
                              stop.params().proxy_outbound_sequence_length);
}

bool NodeLink::OnInitiateProxyBypass(const msg::InitiateProxyBypass& request) {
  Ref<Router> router = GetRouter(request.params().sublink);
  if (!router) {
    return true;
  }

  return router->InitiateProxyBypass(*this, request.params().sublink,
                                     request.params().proxy_peer_name,
                                     request.params().proxy_peer_sublink);
}

bool NodeLink::OnBypassProxy(const msg::BypassProxy& bypass) {
  return node_->OnBypassProxy(*this, bypass);
}

bool NodeLink::OnBypassProxyToSameNode(
    const msg::BypassProxyToSameNode& bypass) {
  Ref<Router> router = GetRouter(bypass.params().sublink);
  if (!router) {
    return true;
  }

  Ref<RouterLink> new_link =
      AddRemoteRouterLink(bypass.params().new_sublink,
                          memory().AdoptFragmentRef<RouterLinkState>(
                              bypass.params().new_link_state_fragment),
                          LinkType::kCentral, LinkSide::kB, router);
  return router->BypassProxyWithNewLinkToSameNode(
      std::move(new_link), bypass.params().proxy_inbound_sequence_length);
}

bool NodeLink::OnStopProxyingToLocalPeer(
    const msg::StopProxyingToLocalPeer& stop) {
  Ref<Router> router = GetRouter(stop.params().sublink);
  if (!router) {
    return true;
  }
  return router->StopProxyingToLocalPeer(
      stop.params().proxy_outbound_sequence_length);
}

bool NodeLink::OnProxyWillStop(const msg::ProxyWillStop& will_stop) {
  Ref<Router> router = GetRouter(will_stop.params().sublink);
  if (!router) {
    return true;
  }

  return router->OnProxyWillStop(
      will_stop.params().proxy_inbound_sequence_length);
}

bool NodeLink::OnFlushRouter(const msg::FlushRouter& flush) {
  if (Ref<Router> router = GetRouter(flush.params().sublink)) {
    router->Flush(/*force_bypass_attempt=*/true);
  }
  return true;
}

bool NodeLink::OnRequestMemory(const msg::RequestMemory& request) {
  DriverMemory memory(node_, request.params().size);
  msg::ProvideMemory provide;
  provide.params().size = request.params().size;
  provide.AppendDriverObject(memory.TakeDriverObject(),
                             provide.params().buffer);
  Transmit(provide);
  return true;
}

bool NodeLink::OnProvideMemory(msg::ProvideMemory& provide) {
  DriverMemory memory(provide.TakeDriverObject(provide.params().buffer));
  if (!memory.is_valid()) {
    return false;
  }

  RequestMemoryCallback callback;
  {
    absl::MutexLock lock(&mutex_);
    auto it = pending_memory_requests_.find(provide.params().size);
    if (it == pending_memory_requests_.end()) {
      return false;
    }

    std::list<RequestMemoryCallback>& callbacks = it->second;
    ABSL_ASSERT(!callbacks.empty());
    callback = std::move(callbacks.front());
    callbacks.pop_front();
    if (callbacks.empty()) {
      pending_memory_requests_.erase(it);
    }
  }

  callback(std::move(memory));
  return true;
}

bool NodeLink::OnLogRouteTrace(const msg::LogRouteTrace& log_request) {
  absl::optional<Sublink> sublink = GetSublink(log_request.params().sublink);
  if (!sublink) {
    return true;
  }

  sublink->receiver->AcceptLogRouteTraceFrom(sublink->router_link->GetType());
  return true;
}

bool NodeLink::OnRelayMessage(msg::RelayMessage& relay) {
  if (node_->type() != Node::Type::kBroker) {
    return false;
  }

  return node_->RelayMessage(remote_node_name_, relay);
}

bool NodeLink::OnAcceptRelayedMessage(msg::AcceptRelayedMessage& relay) {
  if (remote_node_type_ != Node::Type::kBroker) {
    return false;
  }

  return node_->AcceptRelayedMessage(relay);
}

bool NodeLink::OnFlushLink(const msg::FlushLink& flush) {
  // No-op: this message is only sent to elicit a transport notification, which
  // it's already done.
  return true;
}

IpczResult NodeLink::DispatchMessage(const DriverTransport::Message& message) {
  const auto& header =
      *reinterpret_cast<const internal::MessageHeader*>(message.data.data());
  const uint8_t message_id = std::atomic_load_explicit(
      reinterpret_cast<const std::atomic<uint8_t>*>(&header.message_id),
      std::memory_order_acquire);
  switch (message_id) {
// clang-format off
#include "ipcz/message_macros/message_dispatch_macros.h"
#include "ipcz/node_message_defs.h"
#include "ipcz/message_macros/undef_message_macros.h"
      // clang-format on

    default:
      DLOG(WARNING) << "Ignoring unknown transport message with ID "
                    << static_cast<int>(header.message_id);
      break;
  }

  return IPCZ_RESULT_OK;
}

bool NodeLink::AcceptParcelWithoutDriverObjects(SublinkId for_sublink,
                                                Parcel& p) {
  const auto key = std::make_tuple(for_sublink, p.sequence_number());
  Parcel parcel_with_driver_objects;
  {
    absl::MutexLock lock(&mutex_);
    auto [it, ok] = partial_parcels_.try_emplace(key, std::move(p));
    if (ok) {
      return true;
    }
    parcel_with_driver_objects = std::move(it->second);
    partial_parcels_.erase(it);
  }

  return AcceptSplitParcel(for_sublink, parcel_with_driver_objects, p);
}

bool NodeLink::AcceptParcelDriverObjects(SublinkId for_sublink, Parcel& p) {
  const auto key = std::make_tuple(for_sublink, p.sequence_number());
  Parcel parcel_with_data_and_portals;
  {
    absl::MutexLock lock(&mutex_);
    auto [it, ok] = partial_parcels_.try_emplace(key, std::move(p));
    if (ok) {
      return true;
    }
    parcel_with_data_and_portals = std::move(it->second);
    partial_parcels_.erase(it);
  }

  return AcceptSplitParcel(for_sublink, p, parcel_with_data_and_portals);
}

bool NodeLink::AcceptSplitParcel(SublinkId for_sublink,
                                 Parcel& parcel_with_driver_objects,
                                 Parcel& parcel_with_everything_else) {
  // The parcel with no driver objects should still have an object attachemnt
  // slot reserved for every relayed driver object.
  if (parcel_with_everything_else.num_objects() <
      parcel_with_driver_objects.num_objects()) {
    return false;
  }

  // Fill in all the object gaps in the data-only parcel with the boxed objects
  // from the driver objects parcel.
  auto remaining_driver_objects = parcel_with_driver_objects.objects_view();
  for (auto& object : parcel_with_everything_else.objects_view()) {
    if (object) {
      continue;
    }

    if (remaining_driver_objects.empty()) {
      return false;
    }

    object = std::move(remaining_driver_objects[0]);
    remaining_driver_objects.remove_prefix(1);
  }

  // At least one driver object was unclaimed by the data half of the parcel.
  // That's not right.
  if (!remaining_driver_objects.empty()) {
    return false;
  }

  return AcceptCompleteParcel(for_sublink, parcel_with_everything_else);
}

bool NodeLink::AcceptCompleteParcel(SublinkId for_sublink, Parcel& parcel) {
  absl::optional<Sublink> sublink = GetSublink(for_sublink);
  if (!sublink) {
    DVLOG(4) << "Dropping " << parcel.Describe() << " at "
             << local_node_name_.ToString() << ", arriving from "
             << remote_node_name_.ToString() << " via unknown sublink "
             << for_sublink;
    return true;
  }
  const LinkType link_type = sublink->router_link->GetType();
  if (link_type == LinkType::kCentral ||
      link_type == LinkType::kPeripheralOutward) {
    DVLOG(4) << "Accepting inbound " << parcel.Describe() << " at "
             << sublink->router_link->Describe();
    return sublink->receiver->AcceptInboundParcel(parcel);
  } else {
    ABSL_ASSERT(link_type == LinkType::kPeripheralInward);
    DVLOG(4) << "Accepting outbound " << parcel.Describe() << " at "
             << sublink->router_link->Describe();
    return sublink->receiver->AcceptOutboundParcel(parcel);
  }
}

NodeLink::Sublink::Sublink(Ref<RemoteRouterLink> router_link,
                           Ref<Router> receiver)
    : router_link(std::move(router_link)), receiver(std::move(receiver)) {}

NodeLink::Sublink::Sublink(Sublink&&) = default;

NodeLink::Sublink::Sublink(const Sublink&) = default;

NodeLink::Sublink& NodeLink::Sublink::operator=(Sublink&&) = default;

NodeLink::Sublink& NodeLink::Sublink::operator=(const Sublink&) = default;

NodeLink::Sublink::~Sublink() = default;

NodeLink::IncomingMessage::IncomingMessage() = default;

NodeLink::IncomingMessage::IncomingMessage(IncomingMessage&&) = default;

NodeLink::IncomingMessage& NodeLink::IncomingMessage::operator=(
    IncomingMessage&&) = default;

NodeLink::IncomingMessage::~IncomingMessage() = default;

}  // namespace ipcz
