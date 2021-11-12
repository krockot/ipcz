// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node_link.h"

#include <tuple>
#include <utility>
#include <vector>

#include "core/message_internal.h"
#include "core/node.h"
#include "core/node_messages.h"
#include "core/node_name.h"
#include "core/parcel.h"
#include "core/portal.h"
#include "core/portal_descriptor.h"
#include "core/portal_link_state.h"
#include "core/sequence_number.h"
#include "core/side.h"
#include "core/trap_event_dispatcher.h"
#include "debug/log.h"
#include "os/channel.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/numeric/int128.h"

namespace ipcz {
namespace core {

namespace {

struct IPCZ_ALIGN(8) AcceptParcelHeader {
  internal::MessageHeader message_header;
  RouteId route;
  SequenceNumber sequence_number;
  uint32_t num_bytes;
  uint32_t num_portals;
  uint32_t num_os_handles;
};

}  // namespace

NodeLink::NodeLink(Node& node,
                   os::Channel channel,
                   os::Process remote_process,
                   Node::Type remote_node_type)
    : node_(mem::WrapRefCounted(&node)),
      remote_node_type_(remote_node_type),
      channel_(std::move(channel)),
      remote_process_(std::move(remote_process)) {}

NodeLink::NodeLink(Node& node,
                   const NodeName& local_name,
                   const NodeName& remote_name,
                   os::Channel channel,
                   os::Memory::Mapping link_state_mapping)
    : node_(mem::WrapRefCounted(&node)),
      remote_node_type_(Node::Type::kNormal),
      local_name_(local_name),
      remote_name_(remote_name),
      remote_protocol_version_(0),
      channel_(std::move(channel)),
      link_state_mapping_(std::move(link_state_mapping)) {}

NodeLink::~NodeLink() = default;

void NodeLink::Listen() {
  absl::MutexLock lock(&mutex_);
  channel_.Listen([this](os::Channel::Message message) {
    return this->OnMessage(message);
  });
}

mem::Ref<Portal> NodeLink::Invite(const NodeName& local_name,
                                  const NodeName& remote_name) {
  os::Memory portal_link_state_memory(sizeof(PortalLinkState));
  os::Memory::Mapping portal_link_state_mapping =
      portal_link_state_memory.Map();
  PortalLinkState::Initialize(portal_link_state_mapping.base());

  os::Memory node_link_state_memory(sizeof(NodeLinkState));
  os::Memory::Mapping node_link_state_mapping = node_link_state_memory.Map();
  NodeLinkState& node_link_state =
      NodeLinkState::Initialize(node_link_state_mapping.base());

  RouteId route;
  mem::Ref<Portal> portal = mem::MakeRefCounted<Portal>(node_, Side::kLeft);
  mem::Ref<PortalLink> portal_link;
  msg::InviteNode invitation;
  {
    absl::MutexLock lock(&mutex_);
    do {
      route = node_link_state.AllocateRoutes(1);
    } while (!AssignRoute(route, portal));
    local_name_ = local_name;
    remote_name_ = remote_name;
    link_state_mapping_ = std::move(node_link_state_mapping);
    invitation.params.protocol_version = msg::kProtocolVersion;
    invitation.params.source_name = local_name;
    invitation.params.target_name = remote_name;
    invitation.params.route = route;
    invitation.handles.node_link_state_memory =
        node_link_state_memory.TakeHandle();
    invitation.handles.portal_link_state_memory =
        portal_link_state_memory.TakeHandle();
  }

  Send(invitation,
       [link = mem::WrapRefCounted(this)](const msg::InviteNode_Reply* reply) {
         if (!reply || !reply->params.accepted) {
           // Newer versions may tolerate invitation rejection, but it's the
           // only handshake mechanism we have in v0. Treat this as a validation
           // failure.
           return false;
         }

         link->SetRemoteProtocolVersion(reply->params.protocol_version);
         return true;
       });

  portal->ActivateFromBuffering(mem::MakeRefCounted<PortalLink>(
      mem::WrapRefCounted(this), route, std::move(portal_link_state_mapping)));
  return portal;
}

mem::Ref<Portal> NodeLink::AwaitInvitation() {
  mem::Ref<Portal> portal = mem::MakeRefCounted<Portal>(node_, Side::kRight);

  absl::MutexLock lock(&mutex_);
  portal_awaiting_invitation_ = portal;
  return portal;
}

void NodeLink::RequestIntroduction(const NodeName& name,
                                   RequestIntroductionCallback callback) {
  {
    absl::MutexLock lock(&mutex_);
    auto result = pending_introductions_.try_emplace(
        name, std::vector<RequestIntroductionCallback>());
    result.first->second.push_back(std::move(callback));
    if (!result.second) {
      // There's already a request in-flight for the named node, so no need to
      // send another.
      return;
    }
  }

  msg::RequestIntroduction request;
  request.params.name = name;
  Send(request);
}

mem::Ref<PortalLink> NodeLink::BypassProxyToPortal(const NodeName& proxy_name,
                                                   RouteId proxy_route,
                                                   absl::uint128 key,
                                                   mem::Ref<Portal> portal) {
  // TODO: portal link state should be allocated within the NodeLinkState
  // or an auxilliary NodeLink buffer for overflow
  os::Memory link_state_memory(sizeof(PortalLinkState));
  os::Memory::Mapping link_state_mapping = link_state_memory.Map();
  {
    // The other side may not be buffering, but we assume it us until it can
    // update its state to reflect reality. The purpose is to avoid this side
    // becoming a half-proxy before we know it's safe. If this side moves in the
    // interim it will become a full proxy instead.
    PortalLinkState::Locked state(
        PortalLinkState::Initialize(link_state_mapping.base()), portal->side());
    state.this_side().routing_mode = RoutingMode::kActive;
    state.other_side().routing_mode = RoutingMode::kBuffering;
  }

  const RouteId new_route = AllocateRoutes(1);
  {
    absl::MutexLock lock(&mutex_);
    AssignRoute(new_route, portal);
  }

  msg::BypassProxy m;
  m.params.proxy_name = proxy_name;
  m.params.proxy_route = proxy_route;
  m.params.new_route = new_route;
  m.params.sender_side = portal->side();
  m.params.key = key;
  m.handles.new_link_state_memory = link_state_memory.TakeHandle();
  Send(m);
  return mem::MakeRefCounted<PortalLink>(mem::WrapRefCounted(this), new_route,
                                         std::move(link_state_mapping));
}

void NodeLink::StopProxying(RouteId route, SequenceNumber sequence_length) {
  msg::StopProxying m;
  m.params.route = route;
  m.params.sequence_length = sequence_length;
  Send(m);
}

void NodeLink::SetRemoteProtocolVersion(uint32_t version) {
  absl::MutexLock lock(&mutex_);
  remote_protocol_version_ = version;
}

void NodeLink::Send(absl::Span<uint8_t> data, absl::Span<os::Handle> handles) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(channel_.is_valid());
  ABSL_ASSERT(data.size() >= sizeof(internal::MessageHeader));
  channel_.Send(os::Channel::Message(os::Channel::Data(data), handles));
}

void NodeLink::SendWithReplyHandler(absl::Span<uint8_t> data,
                                    absl::Span<os::Handle> handles,
                                    GenericReplyHandler reply_handler) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(channel_.is_valid());
  ABSL_ASSERT(data.size() >= sizeof(internal::MessageHeader));
  internal::MessageHeader& header =
      *reinterpret_cast<internal::MessageHeader*>(data.data());
  header.request_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
  PendingReply pending_reply(header.message_id, std::move(reply_handler));
  while (
      !header.request_id ||
      !pending_replies_.try_emplace(header.request_id, std::move(pending_reply))
           .second) {
    header.request_id =
        next_request_id_.fetch_add(1, std::memory_order_relaxed);
  }
  channel_.Send(os::Channel::Message(os::Channel::Data(data), handles));
}

void NodeLink::SendParcel(RouteId route, Parcel& parcel) {
  // Build small messages on the stack.
  absl::InlinedVector<uint8_t, 256> serialized_data;

  // big ad hoc mess of custom serialization. balanced by deserialization in
  // OnAcceptParcel.

  const size_t num_portals = parcel.portals_view().size();
  const size_t num_os_handles = parcel.os_handles_view().size();
  const size_t serialized_size =
      sizeof(AcceptParcelHeader) + parcel.data_view().size() +
      num_portals * sizeof(PortalDescriptor) +
      (num_os_handles + num_portals) * sizeof(internal::OSHandleData);
  serialized_data.resize(serialized_size);

  auto& header = *reinterpret_cast<AcceptParcelHeader*>(serialized_data.data());
  header.message_header.size = sizeof(header.message_header);
  header.message_header.message_id = msg::kAcceptParcelId;
  header.route = route;
  header.sequence_number = parcel.sequence_number();
  header.num_bytes = parcel.data_view().size();
  header.num_portals = num_portals;
  header.num_os_handles = num_portals + num_os_handles;
  auto* data = reinterpret_cast<uint8_t*>(&header + 1);
  memcpy(data, parcel.data_view().data(), parcel.data_view().size());
  auto* descriptors =
      reinterpret_cast<PortalDescriptor*>(data + header.num_bytes);

  const absl::Span<mem::Ref<Portal>> portals = parcel.portals_view();
  const size_t first_route_id = AllocateRoutes(num_portals);
  std::vector<os::Handle> os_handles;
  std::vector<mem::Ref<PortalLink>> new_links(num_portals);
  os_handles.reserve(num_os_handles + num_portals);
  for (size_t i = 0; i < num_portals; ++i) {
    os::Memory link_state_memory(sizeof(PortalLinkState));
    os::Memory::Mapping link_state = link_state_memory.Map();
    PortalLinkState::Initialize(link_state.base());
    os_handles.push_back(link_state_memory.TakeHandle());
    // TODO: populate OSHandleData too

    const RouteId route = first_route_id + i;
    descriptors[i].route = route;
    portals[i] = portals[i]->Serialize(descriptors[i]);
    new_links[i] = mem::MakeRefCounted<PortalLink>(
        mem::WrapRefCounted(this), route, std::move(link_state));

    // It's important to assign the route to a Portal before we transmit this
    // parcel, in case the receiver immediately sends back messages on the new
    // route.
    absl::MutexLock lock(&mutex_);
    bool assigned = AssignRoute(route, portals[i]);
    ABSL_ASSERT(assigned);
  }

  auto* handle_data =
      reinterpret_cast<internal::OSHandleData*>(descriptors + num_portals);
  for (size_t i = 0; i < num_os_handles; ++i) {
    auto& data = handle_data[num_portals + i];
    auto& handle = parcel.os_handles_view()[i];
    ABSL_ASSERT(handle.is_valid());
    data.header.size = sizeof(internal::StructHeader);
    data.header.version = 0;
#if defined(OS_POSIX)
    data.type = internal::OSHandleDataType::kFileDescriptor;
    data.value = i + 1;
#else
    data.type = internal::OSHandleDataType::kNone;
#endif
    os_handles.push_back(std::move(handle));
  }

  Send(absl::MakeSpan(serialized_data), absl::MakeSpan(os_handles));

  // Don't set links on the local portals until we've sent the parcel, since
  // portals may immediately start sending messages pertaining to their route.
  // The route isn't establlished on the remote side until the parcel above is
  // received there.
  for (size_t i = 0; i < num_portals; ++i) {
    if (descriptors[i].route_is_peer) {
      portals[i]->ActivateFromBuffering(new_links[i]);
    } else {
      portals[i]->BeginForwardProxying(new_links[i]);
    }
  }
}

void NodeLink::SendPeerClosed(RouteId route, SequenceNumber sequence_length) {
  msg::PeerClosed m;
  m.params.route = route;
  m.params.sequence_length = sequence_length;
  Send(m);
}

RouteId NodeLink::AllocateRoutes(size_t count) {
  absl::MutexLock lock(&mutex_);

  // Routes must only be allocated once we're a fully functioning link.
  ABSL_ASSERT(link_state_mapping_.is_valid());
  return link_state_mapping_.As<NodeLinkState>()->AllocateRoutes(count);
}

void NodeLink::DisconnectRoute(RouteId route) {
  absl::MutexLock lock(&mutex_);
  routes_.erase(route);
}

bool NodeLink::AssignRoute(RouteId id, const mem::Ref<Portal>& portal) {
  mutex_.AssertHeld();
  auto result = routes_.try_emplace(id, portal);
  return result.second;
}

mem::Ref<Portal> NodeLink::GetPortalForRoute(RouteId id) {
  absl::MutexLock lock(&mutex_);
  auto it = routes_.find(id);
  if (it == routes_.end()) {
    return nullptr;
  }
  return it->second;
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

  if (header.is_reply) {
    return OnReply(message);
  }

  switch (header.message_id) {
#include "core/message_macros/message_dispatch_macros.h"
#include "core/node_message_defs.h"

#include "core/message_macros/undef_message_macros.h"

    case msg::kAcceptParcelId:
      return OnAcceptParcel(message);

    default:
      // Unknown message types may come from clients using a newer ipcz version.
      // If they expect a reply, reply to indicate that we don't know what
      // they're talking about. Otherwise we silently ignore the message.
      if (header.expects_reply) {
        internal::MessageHeader nope{sizeof(nope)};
        nope.message_id = header.message_id;
        nope.wont_reply = true;
        nope.request_id = header.request_id;
        Send(absl::MakeSpan(reinterpret_cast<uint8_t*>(&nope), sizeof(nope)));
      }
      return true;
  }
}

bool NodeLink::OnReply(os::Channel::Message message) {
  // TODO: make sure we get a validated header from OnMessage(), and separately
  // copy message data
  const auto& header =
      *reinterpret_cast<const internal::MessageHeader*>(message.data.data());

  PendingReply pending_reply;
  {
    absl::MutexLock lock(&mutex_);
    auto it = pending_replies_.find(header.request_id);
    if (it == pending_replies_.end()) {
      // No reply expected with this ID. Oops! Validation failure.
      return false;
    }
    pending_reply = std::move(it->second);
    pending_replies_.erase(it);
  }

  // Not the message type we were expecting for this reply. Reject!
  if (header.message_id != pending_reply.message_id) {
    return false;
  }

  // The receiving Node did not understand our request.
  if (header.wont_reply) {
    pending_reply.handler(nullptr);
    return true;
  }

  switch (header.message_id) {
#include "core/message_macros/message_reply_dispatch_macros.h"
#include "core/node_message_defs.h"

#include "core/message_macros/undef_message_macros.h"

    default:
      // Replies for unrecognized message IDs don't make sense. Reject!
      return false;
  }
}

bool NodeLink::OnInviteNode(msg::InviteNode& m) {
  os::Memory node_link_state(std::move(m.handles.node_link_state_memory),
                             sizeof(NodeLinkState));
  os::Memory portal_link_state(std::move(m.handles.portal_link_state_memory),
                               sizeof(PortalLinkState));
  mem::Ref<Portal> portal;
  {
    absl::MutexLock lock(&mutex_);
    if (remote_node_type_ == Node::Type::kBroker &&
        portal_awaiting_invitation_ && !remote_name_.is_valid() &&
        !local_name_.is_valid()) {
      remote_name_ = m.params.source_name;
      local_name_ = m.params.target_name;
      link_state_mapping_ = node_link_state.Map();
      if (node_->AcceptInvitationFromBroker(m.params.source_name,
                                            m.params.target_name) &&
          AssignRoute(m.params.route, portal_awaiting_invitation_)) {
        portal = std::move(portal_awaiting_invitation_);
      }
    }
  }

  const bool accepted = portal != nullptr;
  msg::InviteNode_Reply reply;
  reply.header.request_id = m.header.request_id;
  reply.params.protocol_version = msg::kProtocolVersion;
  reply.params.accepted = accepted;
  Send(reply);

  if (accepted) {
    // Note that we don't set the portal's active link until after we've replied
    // to accept the invitation. This is because the portal may immediately
    // initiate communication over the route within ActivateFromBuffering(),
    // e.g. to flush outgoing parcels, and we need the invitation reply to
    // arrive first.
    portal->ActivateFromBuffering(mem::MakeRefCounted<PortalLink>(
        mem::WrapRefCounted(this), m.params.route, portal_link_state.Map()));
  } else {
    absl::MutexLock lock(&mutex_);
    routes_.erase(m.params.route);
  }
  return true;
}

bool NodeLink::OnPeerClosed(msg::PeerClosed& m) {
  TrapEventDispatcher dispatcher;
  mem::Ref<Portal> portal = GetPortalForRoute(m.params.route);
  // Note that the portal may have already been closed locally, so we can't
  // treat the route's absence here as an error.
  if (portal) {
    portal->NotifyPeerClosed(m.params.sequence_length, dispatcher);
  }
  return true;
}

bool NodeLink::OnRequestIntroduction(msg::RequestIntroduction& m) {
  NodeName requestor_name;
  {
    absl::MutexLock lock(&mutex_);
    requestor_name = remote_name_;
  }

  mem::Ref<NodeLink> peer = node_->GetLink(m.params.name);
  if (!node_->is_broker() || !peer) {
    // Give a response with no handles, indicating that the introduction failed.
    msg::IntroduceNode introduction;
    introduction.params.name = m.params.name;
    Send(introduction);
    return true;
  }

  os::Channel left, right;
  std::tie(left, right) = os::Channel::CreateChannelPair();

  os::Memory link_state_memory(sizeof(NodeLinkState));
  os::Memory::Mapping link_state_mapping = link_state_memory.Map();
  NodeLinkState::Initialize(link_state_mapping.base());

  msg::IntroduceNode requestor_intro;
  requestor_intro.params.name = m.params.name;
  requestor_intro.handles.channel = left.TakeHandle();
  requestor_intro.handles.link_state_memory =
      link_state_memory.Clone().TakeHandle();
  Send(requestor_intro);

  msg::IntroduceNode peer_intro;
  peer_intro.params.name = requestor_name;
  peer_intro.handles.channel = right.TakeHandle();
  peer_intro.handles.link_state_memory = link_state_memory.TakeHandle();
  peer->Send(peer_intro);
  return true;
}

bool NodeLink::OnIntroduceNode(msg::IntroduceNode& m) {
  if (remote_node_type_ != Node::Type::kBroker) {
    return false;
  }

  if (!m.handles.channel.is_valid() ||
      !m.handles.link_state_memory.is_valid()) {
    DLOG(ERROR) << "Could not get introduction to " << m.params.name.ToString();
    return true;
  }

  mem::Ref<NodeLink> link;
  {
    absl::MutexLock lock(&mutex_);
    link = mem::MakeRefCounted<NodeLink>(
        *node_, local_name_, m.params.name,
        os::Channel(std::move(m.handles.channel)),
        os::Memory(std::move(m.handles.link_state_memory),
                   sizeof(NodeLinkState))
            .Map());
  }

  if (!node_->AddLink(m.params.name, link)) {
    // This introduction may have raced with a previous one and we may already
    // have a link to the node. In that case, discard the link we just created
    // and use the established link instead.
    //
    // It's also possible that in between the above failure and the GetLink()
    // call here, we lose our connection to the named node. In that case `link`
    // will be null and we'll behave below as if the introduction failed.
    link = node_->GetLink(m.params.name);
  } else {
    link->Listen();
  }

  std::vector<RequestIntroductionCallback> callbacks;
  {
    absl::MutexLock lock(&mutex_);
    auto it = pending_introductions_.find(m.params.name);
    if (it == pending_introductions_.end()) {
      return true;
    }

    callbacks = std::move(it->second);
    it->second.clear();
  }
  for (auto& callback : callbacks) {
    callback(link);
  }

  return true;
}

bool NodeLink::OnBypassProxy(msg::BypassProxy& m) {
  auto new_peer_link = mem::MakeRefCounted<PortalLink>(
      mem::WrapRefCounted(this), m.params.new_route,
      os::Memory(std::move(m.handles.new_link_state_memory),
                 sizeof(PortalLinkState))
          .Map());

  mem::Ref<NodeLink> link_to_proxy = node_->GetLink(m.params.proxy_name);
  mem::Ref<Portal> portal;
  if (link_to_proxy) {
    portal = link_to_proxy->GetPortalForRoute(m.params.proxy_route);
  }

  if (portal && portal->ReplacePeerLink(m.params.key, new_peer_link)) {
    absl::MutexLock lock(&mutex_);
    AssignRoute(m.params.new_route, portal);
    return true;
  }

  // We can't accept the new peer. Don't signal a validation failure, but let
  // them know we're toast.
  PortalLinkState::Locked state(new_peer_link->state(),
                                Opposite(m.params.sender_side));
  state.this_side().routing_mode = RoutingMode::kClosed;
  return true;
}

bool NodeLink::OnStopProxying(msg::StopProxying& m) {
  mem::Ref<Portal> portal = GetPortalForRoute(m.params.route);
  if (!portal) {
    return true;
  }

  portal->StopProxying(m.params.sequence_length);
  return true;
}

bool NodeLink::OnAcceptParcel(os::Channel::Message m) {
  if (m.data.size() < sizeof(AcceptParcelHeader)) {
    return false;
  }

  const auto& header =
      *reinterpret_cast<const AcceptParcelHeader*>(m.data.data());
  const uint32_t num_bytes = header.num_bytes;
  const uint32_t num_portals = header.num_portals;
  const uint32_t num_os_handles = header.num_os_handles;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&header + 1);
  const auto* descriptors =
      reinterpret_cast<const PortalDescriptor*>(bytes + num_bytes);

  // const auto* handle_data =
  //     reinterpret_cast<const internal::OSHandleData*>(
  //         descriptors + num_portals);
  if (num_os_handles < num_portals) {
    return false;
  }
  if (m.handles.size() != num_os_handles) {
    return false;
  }

  Parcel::PortalVector portals(num_portals);
  {
    for (size_t i = 0; i < num_portals; ++i) {
      os::Memory link_state_memory(std::move(m.handles[i]),
                                   sizeof(PortalLinkState));
      portals[i] =
          Portal::DeserializeNew(node_, mem::WrapRefCounted(this),
                                 link_state_memory.Map(), descriptors[i]);
    }

    absl::MutexLock lock(&mutex_);
    for (size_t i = 0; i < num_portals; ++i) {
      if (!AssignRoute(descriptors[i].route, portals[i])) {
        return false;
      }
    }
  }

  std::vector<os::Handle> os_handles;
  os_handles.reserve(m.handles.size() - num_portals);
  for (size_t i = num_portals; i < m.handles.size(); ++i) {
    os_handles.push_back(std::move(m.handles[i]));
  }

  TrapEventDispatcher dispatcher;
  Parcel parcel(header.sequence_number);
  parcel.SetData(std::vector<uint8_t>(bytes, bytes + num_bytes));
  parcel.SetPortals(std::move(portals));
  parcel.SetOSHandles(std::move(os_handles));

  mem::Ref<Portal> receiver = GetPortalForRoute(header.route);
  if (!receiver) {
    return true;
  }

  return receiver->AcceptParcelFromLink(parcel, dispatcher);
}

NodeLink::PendingReply::PendingReply() = default;

NodeLink::PendingReply::PendingReply(uint8_t message_id,
                                     GenericReplyHandler handler)
    : message_id(message_id), handler(handler) {}

NodeLink::PendingReply::PendingReply(PendingReply&&) = default;

NodeLink::PendingReply& NodeLink::PendingReply::operator=(PendingReply&&) =
    default;

NodeLink::PendingReply::~PendingReply() = default;

}  // namespace core
}  // namespace ipcz
