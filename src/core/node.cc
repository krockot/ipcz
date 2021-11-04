// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node.h"

#include <utility>

#include "core/node_link.h"
#include "core/node_messages.h"
#include "core/node_name.h"
#include "core/portal.h"
#include "debug/log.h"
#include "mem/ref_counted.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {
namespace core {

Node::Node(Type type) : type_(type) {
  if (type_ == Type::kBroker) {
    name_ = NodeName(NodeName::kRandom);
  }
}

Node::~Node() = default;

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

  const NodeName their_name{NodeName::kRandom};
  NodeName our_name;
  {
    absl::MutexLock lock(&mutex_);
    our_name = name_;
  }

  mem::Ref<NodeLink> link = mem::MakeRefCounted<NodeLink>(
      *this, std::move(channel), std::move(process), Type::kNormal);
  out_portal = link->Invite(our_name, their_name);

  absl::MutexLock lock(&mutex_);
  node_links_[their_name] = link;
  link->Listen();
  return IPCZ_RESULT_OK;
}

IpczResult Node::AcceptRemotePortal(os::Channel channel,
                                    mem::Ref<Portal>& out_portal) {
  ABSL_ASSERT(type_ != Type::kBroker);

  mem::Ref<NodeLink> link;
  {
    absl::MutexLock lock(&mutex_);
    if (broker_link_) {
      return IPCZ_RESULT_FAILED_PRECONDITION;
    }
    link = mem::MakeRefCounted<NodeLink>(*this, std::move(channel),
                                         os::Process(), Type::kBroker);
    broker_link_ = link;
  }

  out_portal = link->AwaitInvitation();
  link->Listen();
  return IPCZ_RESULT_OK;
}

bool Node::AcceptInvitationFromBroker(const NodeName& broker_name,
                                      const NodeName& our_name) {
  if (type_ == Type::kBroker) {
    return false;
  }

  absl::MutexLock lock(&mutex_);
  if (name_.is_valid()) {
    return false;
  }

  name_ = our_name;
  node_links_[broker_name] = broker_link_;
  return true;
}

void Node::ShutDown() {}

}  // namespace core
}  // namespace ipcz
