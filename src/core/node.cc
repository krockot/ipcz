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
#include "util/handle_util.h"
namespace ipcz {
namespace core {

Node::Node(Type type, const IpczDriver& driver, IpczDriverHandle driver_node)
    : type_(type), driver_(driver), driver_node_(driver_node) {
  if (type_ == Type::kBroker) {
    name_ = NodeName(NodeName::kRandom);
  }
}

Node::~Node() = default;

mem::Ref<NodeLink> Node::GetBrokerLink() {
  absl::MutexLock lock(&mutex_);
  return broker_link_;
}

mem::Ref<NodeLink> Node::GetLink(const NodeName& name) {
  absl::MutexLock lock(&mutex_);
  auto it = node_links_.find(name);
  if (it == node_links_.end()) {
    return nullptr;
  }
  return it->second;
}

bool Node::AddLink(const NodeName& name, const mem::Ref<NodeLink>& link) {
  absl::MutexLock lock(&mutex_);
  auto result = node_links_.try_emplace(name, link);
  return result.second;
}

void Node::EstablishLink(
    const NodeName& name,
    std::function<void(const mem::Ref<NodeLink>& link)> callback) {
  mem::Ref<NodeLink> link;
  {
    absl::MutexLock lock(&mutex_);
    auto it = node_links_.find(name);
    if (it != node_links_.end()) {
      link = it->second;
    }
  }

  if (link) {
    callback(link);
  } else if (type_ == Type::kBroker) {
    LOG(ERROR) << "broker cannot automatically establish link to unknown node";
    callback(nullptr);
  } else {
    GetBrokerLink()->RequestIntroduction(name, callback);
  }
}

IpczResult Node::CreateTransports(Transport::Descriptor& first,
                                  Transport::Descriptor& second) {
  IpczDriverHandle transports[2];
  IpczResult result = driver_.CreateTransports(
      driver_node_, IPCZ_NO_FLAGS, nullptr, &transports[0], &transports[1]);
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  Transport::Descriptor descriptors[2];
  for (size_t i = 0; i < 2; ++i) {
    uint32_t num_bytes = 0;
    uint32_t num_os_handles = 0;
    result = driver_.SerializeTransport(transports[i], IPCZ_NO_FLAGS, nullptr,
                                        nullptr, &num_bytes, nullptr,
                                        &num_os_handles);
    if (result == IPCZ_RESULT_RESOURCE_EXHAUSTED) {
      std::vector<IpczOSHandle> os_handles(num_os_handles);
      for (IpczOSHandle& handle : os_handles) {
        handle.size = sizeof(handle);
      }
      descriptors[i].data.resize(num_bytes);
      result = driver_.SerializeTransport(
          transports[i], IPCZ_NO_FLAGS, nullptr, descriptors[i].data.data(),
          &num_bytes, os_handles.data(), &num_os_handles);
      ABSL_ASSERT(result == IPCZ_RESULT_OK);

      descriptors[i].handles.resize(num_os_handles);
      for (size_t j = 0; j < num_os_handles; ++j) {
        descriptors[i].handles[j] = os::Handle::FromIpczOSHandle(os_handles[j]);
      }
    }
  }

  first = std::move(descriptors[0]);
  second = std::move(descriptors[1]);
  return IPCZ_RESULT_OK;
}

IpczResult Node::DeserializeTransport(const os::Process& remote_process,
                                      Transport::Descriptor& descriptor,
                                      IpczDriverHandle* driver_transport) {
  std::vector<IpczOSHandle> os_handles(descriptor.handles.size());
  for (size_t i = 0; i < descriptor.handles.size(); ++i) {
    os_handles[i].size = sizeof(os_handles[i]);
    if (!os::Handle::ToIpczOSHandle(std::move(descriptor.handles[i]),
                                    &os_handles[i])) {
      return IPCZ_RESULT_UNKNOWN;
    }
  }

  IpczOSProcessHandle ipcz_process = {sizeof(ipcz_process)};
  os::Process::ToIpczOSProcessHandle(remote_process.Clone(), ipcz_process);
  return driver_.DeserializeTransport(
      driver_node_, descriptor.data.data(),
      static_cast<uint32_t>(descriptor.data.size()), os_handles.data(),
      static_cast<uint32_t>(os_handles.size()), &ipcz_process, IPCZ_NO_FLAGS,
      nullptr, driver_transport);
}

IpczResult Node::ConnectNode(IpczDriverHandle driver_handle,
                             Type remote_node_type,
                             os::Process process,
                             absl::Span<IpczHandle> initial_portals) {
  const Side side = type_ == Node::Type::kBroker ? Side::kLeft : Side::kRight;
  for (IpczHandle& handle : initial_portals) {
    auto portal = mem::MakeRefCounted<Portal>(mem::WrapRefCounted(this), side);
    handle = ToHandle(portal.release());
  }

  mem::Ref<NodeLink> link;
  if (type_ == Type::kBroker) {
    const NodeName their_name{NodeName::kRandom};
    NodeName our_name;
    {
      absl::MutexLock lock(&mutex_);
      our_name = name_;
    }
    link = mem::MakeRefCounted<NodeLink>(
        *this,
        mem::MakeRefCounted<Transport>(mem::WrapRefCounted(this),
                                       driver_handle),
        std::move(process), Type::kNormal);

    {
      absl::MutexLock lock(&mutex_);
      node_links_[their_name] = link;
    }
    link->Invite(our_name, their_name, initial_portals);
  } else {
    link = mem::MakeRefCounted<NodeLink>(
        *this,
        mem::MakeRefCounted<Transport>(mem::WrapRefCounted(this),
                                       driver_handle),
        os::Process(), Type::kBroker);
    link->AwaitInvitation(initial_portals);

    absl::MutexLock lock(&mutex_);
    if (broker_link_) {
      return IPCZ_RESULT_FAILED_PRECONDITION;
    }
    broker_link_ = link;
  }

  link->Activate();
  return IPCZ_RESULT_OK;
}

Portal::Pair Node::OpenPortals() {
  return Portal::CreateLocalPair(mem::WrapRefCounted(this));
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

void Node::ShutDown() {
  absl::flat_hash_map<NodeName, mem::Ref<NodeLink>> node_links;
  {
    absl::MutexLock lock(&mutex_);
    broker_link_.reset();
    node_links = std::move(node_links_);
    node_links_.clear();
  }

  for (const auto& entry : node_links) {
    entry.second->Deactivate();
  }
}

}  // namespace core
}  // namespace ipcz
