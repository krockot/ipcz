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
    : router_(node.router_), lock_(&node.router_mutex_) {}

Node::LockedRouter::~LockedRouter() = default;

Node::Node() = default;

Node::~Node() = default;

void Node::ShutDown() {}

Portal::Pair Node::OpenPortals() {
  return Portal::CreateLocalPair(*this);
}

IpczResult Node::OpenRemotePortal(os::Channel channel,
                                  os::Process process,
                                  mem::Ref<Portal>& out_portal) {
  mem::Ref<Portal> portal = Portal::CreateRouted(*this);
  AddNodeLink(std::move(channel), std::move(process));
  out_portal = std::move(portal);
  return IPCZ_RESULT_OK;
}

IpczResult Node::AcceptRemotePortal(os::Channel channel,
                                    mem::Ref<Portal>& out_portal) {
  mem::Ref<Portal> portal = Portal::CreateRouted(*this);
  AddNodeLink(std::move(channel), os::Process());
  out_portal = std::move(portal);
  return IPCZ_RESULT_OK;
}

mem::Ref<NodeLink> Node::AddNodeLink(os::Channel channel, os::Process process) {
  mem::Ref<NodeLink> link = mem::MakeRefCounted<NodeLink>(
      *this, std::move(channel), std::move(process));
  msg::RequestBrokerLink request;
  link->Send(request);
  anonymous_node_links_.emplace(link);
  return link;
}

}  // namespace core
}  // namespace ipcz
