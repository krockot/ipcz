// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node.h"

#include <utility>

#include "core/buffering_portal_backend.h"
#include "core/name.h"
#include "core/node_link.h"
#include "core/node_messages.h"
#include "core/portal.h"
#include "core/portal_control_block.h"
#include "core/routed_portal_backend.h"
#include "debug/log.h"
#include "mem/ref_counted.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {
namespace core {

Node::LockedRouter::LockedRouter(Node& node)
    : router_(node), lock_(&node.mutex_) {}

Node::LockedRouter::~LockedRouter() = default;

Node::Node(Type type) : type_(type) {
  if (type_ == Type::kBroker) {
    name_ = NodeName(Name::kRandom);
  }
}

Node::~Node() = default;

void Node::ShutDown() {}

mem::Ref<NodeLink> Node::GetBrokerLink() {
  absl::MutexLock lock(&mutex_);
  return broker_link_;
}

Portal::Pair Node::OpenPortals() {
  return Portal::CreateLocalPair(*this);
}

IpczResult Node::OpenRemotePortal(os::Channel channel,
                                  os::Process process,
                                  mem::Ref<Portal>& out_portal) {
  // TODO: don't restrict this to broker nodes (maybe?)
  ABSL_ASSERT(type_ == Type::kBroker);

  const NodeName their_node_name{Name::kRandom};
  const PortalName their_portal_name{Name::kRandom};
  NodeName our_node_name;
  const PortalName our_portal_name{Name::kRandom};

  {
    absl::MutexLock lock(&mutex_);
    our_node_name = name_;
  }

  os::Memory control_block_memory(sizeof(PortalControlBlock));
  os::Memory::Mapping control_block_mapping = control_block_memory.Map();

  // By convention, OpenRemotePortal creates a left-side portal.
  //
  // Note that while the remote end may not actually be ready yet, we know it
  // can't be moved elsewhere, so it's safe to begin routing parcels there.
  // Worst case, it may be closed by the time they arrive and they'll be
  // discarded.
  PortalControlBlock& control_block =
      PortalControlBlock::Initialize(control_block_mapping.base());
  memset(&control_block, 0, sizeof(control_block));

  auto backend = std::make_unique<RoutedPortalBackend>(
      our_portal_name, PortalAddress(their_node_name, their_portal_name),
      Side::kLeft, std::move(control_block_mapping));
  mem::Ref<Portal> portal =
      mem::MakeRefCounted<Portal>(*this, std::move(backend));

  mem::Ref<NodeLink> link = mem::MakeRefCounted<NodeLink>(
      *this, std::move(channel), std::move(process), Type::kNormal);
  link->SetRemoteNodeName(their_node_name);

  msg::InviteNode invitation;
  invitation.params.protocol_version = msg::kProtocolVersion;
  invitation.params.your_portal = {their_node_name, their_portal_name};
  invitation.params.broker_portal = {our_node_name, our_portal_name};
  invitation.handles.control_block_memory = control_block_memory.TakeHandle();
  link->Send(invitation, [link](const msg::InviteNode_Reply* reply) {
    if (!reply || !reply->params.accepted) {
      // Newer versions may tolerate invitation rejection, but it's the only
      // handshake mechanism we have in v0. Treat this as a validation failure.
      return false;
    }

    link->SetRemoteProtocolVersion(reply->params.protocol_version);
    return true;
  });

  absl::MutexLock lock(&mutex_);
  node_links_[their_node_name] = link;
  routed_portals_[our_portal_name] = portal;
  out_portal = std::move(portal);
  link->AddRoutedPortal(our_portal_name);
  link->Listen();
  return IPCZ_RESULT_OK;
}

IpczResult Node::AcceptRemotePortal(os::Channel channel,
                                    mem::Ref<Portal>& out_portal) {
  ABSL_ASSERT(type_ != Type::kBroker);

  mem::Ref<NodeLink> link = mem::MakeRefCounted<NodeLink>(
      *this, std::move(channel), os::Process(), Type::kBroker);

  // By convention, AcceptRemotePortal creates a right-side portal.
  mem::Ref<Portal> portal = mem::MakeRefCounted<Portal>(
      *this, std::make_unique<BufferingPortalBackend>(Side::kRight));

  absl::MutexLock lock(&mutex_);
  if (broker_link_) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }
  link->Listen();
  broker_link_ = link;
  portal_waiting_for_invitation_ = portal;
  out_portal = std::move(portal);
  return IPCZ_RESULT_OK;
}

bool Node::AcceptInvitationFromBroker(const PortalAddress& my_address,
                                      const PortalAddress& broker_portal,
                                      os::Memory control_block_memory) {
  if (type_ == Type::kBroker) {
    return false;
  }

  Node::LockedRouter router(*this);
  mutex_.AssertHeld();

  if (!portal_waiting_for_invitation_) {
    return true;
  }

  if (name_.is_valid()) {
    return false;
  }

  name_ = my_address.node();
  node_links_[broker_portal.node()] = broker_link_;

  if (!portal_waiting_for_invitation_->StartRouting(
          router, my_address.portal(), broker_portal,
          control_block_memory.Map())) {
    return false;
  }

  broker_link_->AddRoutedPortal(my_address.portal());
  routed_portals_[my_address.portal()] =
      std::move(portal_waiting_for_invitation_);
  return true;
}

bool Node::AcceptParcel(const PortalName& destination,
                        Parcel& parcel,
                        TrapEventDispatcher& dispatcher) {
  absl::MutexLock lock(&mutex_);
  auto it = routed_portals_.find(destination);
  if (it == routed_portals_.end()) {
    // Portal may have been closed while this parcel was in flight, so it's not
    // necessarily an error. Just discard.
    return true;
  }

  return it->second->AcceptParcel(parcel, dispatcher);
}

bool Node::OnPeerClosed(const PortalName& portal,
                        TrapEventDispatcher& dispatcher) {
  absl::MutexLock lock(&mutex_);
  auto it = routed_portals_.find(portal);
  if (it == routed_portals_.end()) {
    return true;
  }

  return it->second->NotifyPeerClosed(dispatcher);
}

bool Node::RouteParcel(const PortalAddress& destination, Parcel& parcel) {
  mutex_.AssertHeld();

  auto it = node_links_.find(destination.node());
  if (it == node_links_.end()) {
    // We no longer have a link to the destination node. Treat the remote portal
    // as closed by returning failure.
    return false;
  }

  it->second->SendParcel(destination.portal(), parcel);
  return true;
}

bool Node::NotifyPeerClosed(const PortalAddress& destination) {
  mutex_.AssertHeld();
  auto it = node_links_.find(destination.node());
  if (it == node_links_.end()) {
    return false;
  }

  msg::PeerClosed m;
  m.params.local_portal = destination.portal();
  it->second->Send(m);
  return true;
}

}  // namespace core
}  // namespace ipcz
