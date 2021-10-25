// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node.h"

#include <utility>

#include "core/name.h"
#include "core/node_link.h"
#include "core/node_messages.h"
#include "core/portal.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {
namespace core {

Node::LockedRouter::LockedRouter(Node& node)
    : router_(node.router_), lock_(&node.mutex_) {}

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
      *this, std::move(channel), std::move(process));

  NodeName name{Name::kRandom};
  msg::InviteNode invitation;
  invitation.data.high = name.high();
  invitation.data.low = name.low();
  invitation.data.broker_high = name_.high();
  invitation.data.broker_low = name_.low();
  link->Send(invitation);

  mem::Ref<Portal> portal = Portal::CreateRouted(*this);
  out_portal = std::move(portal);
  return IPCZ_RESULT_OK;
}

IpczResult Node::AcceptRemotePortal(os::Channel channel,
                                    mem::Ref<Portal>& out_portal) {
  ABSL_ASSERT(type_ != Type::kBroker);

  mem::Ref<NodeLink> link =
      mem::MakeRefCounted<NodeLink>(*this, std::move(channel), os::Process());

  mem::Ref<Portal> portal = Portal::CreateRouted(*this);
  out_portal = std::move(portal);

  absl::MutexLock lock(&mutex_);
  if (broker_link_) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  broker_link_ = std::move(link);

  return IPCZ_RESULT_OK;
}

}  // namespace core
}  // namespace ipcz
