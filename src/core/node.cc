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
#include "core/routed_portal_backend.h"
#include "debug/log.h"
#include "mem/ref_counted.h"
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

  mem::Ref<NodeLink> link = mem::MakeRefCounted<NodeLink>(
      *this, std::move(channel), std::move(process), Type::kNormal);

  const NodeName their_node_name{Name::kRandom};
  const PortalName their_portal_name{Name::kRandom};
  const PortalName our_portal_name{Name::kRandom};
  msg::InviteNode invitation;
  invitation.params.protocol_version = msg::kProtocolVersion;
  invitation.params.your_portal = {their_node_name, their_portal_name};
  invitation.params.broker_portal = {name_, our_portal_name};
  link->Send(invitation, [link](const msg::InviteNode_Reply* reply) {
    if (!reply->params.accepted) {
      return false;
    }

    link->SetRemoteProtocolVersion(reply->params.protocol_version);
    return true;
  });

  mem::Ref<Portal> portal = mem::MakeRefCounted<Portal>(
      *this,
      std::make_unique<RoutedPortalBackend>(
          our_portal_name, PortalAddress(their_node_name, their_portal_name)));
  out_portal = std::move(portal);

  link->Listen();
  return IPCZ_RESULT_OK;
}

IpczResult Node::AcceptRemotePortal(os::Channel channel,
                                    mem::Ref<Portal>& out_portal) {
  ABSL_ASSERT(type_ != Type::kBroker);

  mem::Ref<NodeLink> link = mem::MakeRefCounted<NodeLink>(
      *this, std::move(channel), os::Process(), Type::kBroker);

  mem::Ref<Portal> portal = mem::MakeRefCounted<Portal>(
      *this, std::make_unique<BufferingPortalBackend>());
  portal_waiting_for_invitation_ = portal;
  out_portal = std::move(portal);

  absl::MutexLock lock(&mutex_);
  if (broker_link_) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }
  link->Listen();
  broker_link_ = link;
  return IPCZ_RESULT_OK;
}

bool Node::AcceptInvitation(const PortalAddress& my_address,
                            const PortalAddress& broker_portal) {
  if (type_ == Type::kBroker) {
    return false;
  }

  absl::MutexLock lock(&mutex_);
  if (!portal_waiting_for_invitation_) {
    return true;
  }

  if (name_.is_valid()) {
    return false;
  }

  name_ = my_address.node();
  return portal_waiting_for_invitation_->StartRouting(my_address.portal(),
                                                      broker_portal);
}

void Node::RouteParcel(const PortalAddress& destination, Parcel& parcel) {
  mutex_.AssertHeld();

  LOG(INFO) << "Routing parcel with " << parcel.data_view().size()
            << " bytes to " << destination.ToString();
}

}  // namespace core
}  // namespace ipcz
