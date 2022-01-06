// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "core/driver_transport.h"
#include "core/link_side.h"
#include "core/link_type.h"
#include "core/message_internal.h"
#include "core/node_connector.h"
#include "core/node_link.h"
#include "core/node_link_memory.h"
#include "core/node_messages.h"
#include "core/portal.h"
#include "core/router.h"
#include "core/routing_id.h"
#include "debug/log.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/handle_util.h"

namespace ipcz {
namespace core {

Node::Node(Type type, const IpczDriver& driver, IpczDriverHandle driver_node)
    : type_(type), driver_(driver), driver_node_(driver_node) {
  if (type_ == Type::kBroker) {
    // Only brokers assign their own names.
    assigned_name_ = NodeName{NodeName::kRandom};
    DVLOG(4) << "Created new broker node " << assigned_name_.ToString();
  } else {
    DVLOG(4) << "Created new non-broker node " << this;
  }
}

Node::~Node() = default;

void Node::ShutDown() {
  absl::flat_hash_map<NodeName, mem::Ref<NodeLink>> node_links;
  {
    absl::MutexLock lock(&mutex_);
    std::swap(node_links_, node_links);
    broker_link_.reset();
  }

  for (const auto& entry : node_links) {
    entry.second->Deactivate();
  }
}

IpczResult Node::ConnectNode(IpczDriverHandle driver_transport,
                             os::Process remote_process,
                             IpczConnectNodeFlags flags,
                             absl::Span<IpczHandle> initial_portals) {
  std::vector<mem::Ref<Portal>> portals(initial_portals.size());
  for (size_t i = 0; i < initial_portals.size(); ++i) {
    auto portal = mem::MakeRefCounted<Portal>(mem::WrapRefCounted(this),
                                              mem::MakeRefCounted<Router>());
    portals[i] = portal;
    initial_portals[i] = ToHandle(portal.release());
  }

  auto transport =
      mem::MakeRefCounted<DriverTransport>(driver_, driver_transport);
  IpczResult result =
      NodeConnector::ConnectNode(mem::WrapRefCounted(this), transport,
                                 std::move(remote_process), flags, portals);
  if (result != IPCZ_RESULT_OK) {
    transport->Release();
    for (mem::Ref<Portal>& portal : portals) {
      mem::Ref<Portal> doomed_portal{mem::RefCounted::kAdoptExistingRef,
                                     portal.get()};
    }
    return result;
  }
  return IPCZ_RESULT_OK;
}

void Node::SetPortalsWaitingForLink(
    const NodeName& node_name,
    absl::Span<const mem::Ref<Portal>> portals) {
  absl::MutexLock lock(&mutex_);

  // TODO: this could be less arbitrary. gist is that an initial NodeLink will
  // have a limited supply of RouterLinkState memory available and we want to
  // keep their allocation simple for bootstrap portals, so we place a hard cap
  // on the number of initial portals we support.
  ABSL_ASSERT(portals.size() < 64);

  DVLOG(4) << "Holding on to " << portals.size() << " portal(s) waiting for a "
           << "link to " << node_name.ToString();

  std::vector<mem::Ref<Portal>> waiting_portals(portals.begin(), portals.end());
  pending_introductions_[node_name].push_back([waiting_portals](
                                                  mem::Ref<NodeLink> link) {
    if (!link) {
      for (const mem::Ref<Portal>& portal : waiting_portals) {
        portal->router()->AcceptRouteClosureFrom(Direction::kOutward, 0);
      }
      return;
    }

    DVLOG(4) << "Upon introduction to " << link->remote_node_name().ToString()
             << ", activating " << waiting_portals.size()
             << " portals on link from " << link->local_node_name().ToString()
             << " to " << link->remote_node_name().ToString();

    for (size_t i = 0; i < waiting_portals.size(); ++i) {
      const mem::Ref<Router> router = waiting_portals[i]->router();
      router->SetOutwardLink(
          link->AddRoute(static_cast<RoutingId>(i),
                         link->memory().GetInitialRouterLinkState(i),
                         LinkType::kCentral, LinkSide::kB, router));
    }
  });
}

Portal::Pair Node::OpenPortals() {
  return Portal::CreatePair(mem::WrapRefCounted(this));
}

NodeName Node::GetAssignedName() {
  absl::MutexLock lock(&mutex_);
  return assigned_name_;
}

mem::Ref<NodeLink> Node::GetLink(const NodeName& name) {
  absl::MutexLock lock(&mutex_);
  auto it = node_links_.find(name);
  if (it == node_links_.end()) {
    return nullptr;
  }
  return it->second;
}

mem::Ref<NodeLink> Node::GetBrokerLink() {
  absl::MutexLock lock(&mutex_);
  return broker_link_;
}

void Node::SetBrokerLink(mem::Ref<NodeLink> link) {
  std::vector<BrokerCallback> callbacks;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(!broker_link_);
    broker_link_ = link;
    std::swap(callbacks, broker_callbacks_);
  }
  for (BrokerCallback& callback : callbacks) {
    callback(link);
  }
}

void Node::SetAssignedName(const NodeName& name) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!assigned_name_.is_valid());
  assigned_name_ = name;
}

bool Node::AddLink(const NodeName& remote_node_name, mem::Ref<NodeLink> link) {
  absl::MutexLock lock(&mutex_);
  auto result = node_links_.try_emplace(remote_node_name, std::move(link));
  return result.second;
}

void Node::EstablishLink(const NodeName& name, EstablishLinkCallback callback) {
  mem::Ref<NodeLink> link;
  {
    absl::MutexLock lock(&mutex_);
    auto it = node_links_.find(name);
    if (it != node_links_.end()) {
      link = it->second;
    }
  }

  if (!link && type_ == Type::kBroker) {
    DLOG(ERROR) << "Broker cannot establish link to unknown node "
                << name.ToString();
    callback(nullptr);
    return;
  }

  if (link) {
    callback(link.get());
    return;
  }

  mem::Ref<NodeLink> broker;
  {
    absl::MutexLock lock(&mutex_);
    if (broker_link_) {
      auto result = pending_introductions_.try_emplace(
          name, std::vector<EstablishLinkCallback>());
      result.first->second.push_back(std::move(callback));
      if (!result.second) {
        // An introduction has already been requested for this name.
        return;
      }

      DVLOG(4) << "Node " << assigned_name_.ToString()
               << " requesting introduction to " << name.ToString();

      broker = broker_link_;
    }
  }

  if (!broker) {
    DLOG(ERROR) << "Non-broker cannot establish link to unknown node "
                << name.ToString() << " without a broker link.";
    callback(nullptr);
    return;
  }

  broker->RequestIntroduction(name);
}

bool Node::OnRequestIndirectBrokerConnection(
    NodeLink& from_node_link,
    uint64_t request_id,
    mem::Ref<DriverTransport> transport,
    os::Process process,
    uint32_t num_initial_portals) {
  if (type_ != Type::kBroker) {
    return false;
  }

  DVLOG(4) << "Broker " << from_node_link.local_node_name().ToString()
           << " received indirect connection request from "
           << from_node_link.remote_node_name().ToString();

  IpczResult result = NodeConnector::ConnectNodeIndirect(
      mem::WrapRefCounted(this), mem::WrapRefCounted(&from_node_link),
      std::move(transport), std::move(process), num_initial_portals,
      [node = mem::WrapRefCounted(this),
       source_link = mem::WrapRefCounted(&from_node_link), num_initial_portals,
       request_id](mem::Ref<NodeLink> new_link, uint32_t num_remote_portals) {
        msg::AcceptIndirectBrokerConnection accept;
        accept.params.request_id = request_id;
        accept.params.success = new_link != nullptr;
        accept.params.num_remote_portals = num_remote_portals;
        accept.params.connected_node_name =
            new_link ? new_link->remote_node_name() : NodeName();
        source_link->Transmit(accept);

        const uint32_t num_portals =
            std::min(num_initial_portals, num_remote_portals);
        os::Memory primary_buffer_memory;
        NodeLinkMemory::Allocate(node, num_portals, primary_buffer_memory);
        std::pair<mem::Ref<DriverTransport>, mem::Ref<DriverTransport>>
            transports = DriverTransport::CreatePair(node->driver(),
                                                     node->driver_node());
        new_link->IntroduceNode(source_link->remote_node_name(),
                                std::move(transports.first),
                                primary_buffer_memory.Clone());
        source_link->IntroduceNode(new_link->remote_node_name(),
                                   std::move(transports.second),
                                   std::move(primary_buffer_memory));
      });
  if (result != IPCZ_RESULT_OK) {
    return false;
  }

  return true;
}

bool Node::OnRequestIntroduction(NodeLink& from_node_link,
                                 const msg::RequestIntroduction& request) {
  if (type_ != Type::kBroker) {
    return false;
  }

  mem::Ref<NodeLink> other_node_link;
  {
    absl::MutexLock lock(&mutex_);
    auto it = node_links_.find(request.params.name);
    if (it != node_links_.end()) {
      other_node_link = it->second;
    }
  }

  if (!other_node_link) {
    from_node_link.IntroduceNode(request.params.name, nullptr, os::Memory());
    return true;
  }

  os::Memory primary_buffer_memory;
  NodeLinkMemory::Allocate(mem::WrapRefCounted(this), /*num_initial_portals=*/0,
                           primary_buffer_memory);
  std::pair<mem::Ref<DriverTransport>, mem::Ref<DriverTransport>> transports =
      DriverTransport::CreatePair(driver_, driver_node_);
  other_node_link->IntroduceNode(from_node_link.remote_node_name(),
                                 std::move(transports.first),
                                 primary_buffer_memory.Clone());
  from_node_link.IntroduceNode(request.params.name,
                               std::move(transports.second),
                               std::move(primary_buffer_memory));
  return true;
}

bool Node::OnIntroduceNode(
    const NodeName& name,
    bool known,
    mem::Ref<NodeLinkMemory> link_memory,
    absl::Span<const uint8_t> serialized_transport_data,
    absl::Span<os::Handle> serialized_transport_handles) {
  mem::Ref<DriverTransport> transport;
  mem::Ref<NodeLink> new_link;
  if (known) {
    transport = DriverTransport::Deserialize(driver_, driver_node_,
                                             serialized_transport_data,
                                             serialized_transport_handles);
    if (transport) {
      absl::MutexLock lock(&mutex_);
      ABSL_ASSERT(assigned_name_.is_valid());
      DVLOG(3) << "Node " << assigned_name_.ToString()
               << " received introduction to " << name.ToString();
      new_link =
          NodeLink::Create(mem::WrapRefCounted(this), assigned_name_, name,
                           Type::kNormal, 0, transport, std::move(link_memory));
    }
  }

  std::vector<EstablishLinkCallback> callbacks;
  {
    absl::MutexLock lock(&mutex_);
    auto result = node_links_.try_emplace(name, new_link);
    if (!result.second) {
      // Already introduced. Nothing to do.
      return true;
    }

    auto it = pending_introductions_.find(name);
    if (it != pending_introductions_.end()) {
      callbacks = std::move(it->second);
      pending_introductions_.erase(it);
    }
  }

  if (transport) {
    transport->Activate();
  }

  for (EstablishLinkCallback& callback : callbacks) {
    callback(new_link.get());
  }
  return true;
}

bool Node::OnBypassProxy(NodeLink& from_node_link,
                         const msg::BypassProxy& bypass) {
  mem::Ref<NodeLink> proxy_node_link = GetLink(bypass.params.proxy_name);
  if (!proxy_node_link) {
    return true;
  }

  mem::Ref<Router> proxy_peer =
      proxy_node_link->GetRouter(bypass.params.proxy_routing_id);
  if (!proxy_peer) {
    DLOG(ERROR) << "Invalid BypassProxy request for unknown link from "
                << proxy_node_link->local_node_name().ToString() << " to "
                << proxy_node_link->remote_node_name().ToString()
                << " on routing ID " << bypass.params.proxy_routing_id;
    return false;
  }

  // By convention, the initiator of a bypass uses the side A of the bypass
  // link. The receiver of the bypass request uses side B. Bypass links always
  // connect one half of their route to the other.
  mem::Ref<RemoteRouterLink> new_peer_link = from_node_link.AddRoute(
      bypass.params.new_routing_id, bypass.params.new_link_state_address,
      LinkType::kCentral, LinkSide::kB, proxy_peer);
  return proxy_peer->BypassProxyWithNewRemoteLink(
      new_peer_link, bypass.params.proxy_outbound_sequence_length);
}

void Node::AddBrokerCallback(BrokerCallback callback) {
  mem::Ref<NodeLink> broker_link;
  {
    absl::MutexLock lock(&mutex_);
    if (!broker_link_) {
      broker_callbacks_.push_back(std::move(callback));
      return;
    }
    broker_link = broker_link_;
  }
  callback(std::move(broker_link));
}

void Node::AllocateSharedMemory(size_t size,
                                AllocateSharedMemoryCallback callback) {
  mem::Ref<NodeLink> delegate;
  {
    absl::MutexLock lock(&mutex_);
    delegate = allocation_delegate_link_;
  }

  if (!delegate) {
    callback(os::Memory(size));
    return;
  }

  delegate->RequestMemory(static_cast<uint32_t>(size), std::move(callback));
}

void Node::SetAllocationDelegate(mem::Ref<NodeLink> link) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!allocation_delegate_link_);
  allocation_delegate_link_ = std::move(link);
}

}  // namespace core
}  // namespace ipcz
