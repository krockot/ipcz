// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node_link.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "core/node.h"
#include "core/node_link_buffer.h"
#include "core/node_messages.h"
#include "core/portal.h"
#include "core/portal_descriptor.h"
#include "core/remote_router_link.h"
#include "core/router.h"
#include "core/router_link.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace core {

NodeLink::NodeLink(mem::Ref<Node> node,
                   const NodeName& remote_node_name,
                   uint32_t remote_protocol_version,
                   mem::Ref<DriverTransport> transport,
                   os::Memory::Mapping link_memory)
    : node_(std::move(node)),
      remote_node_name_(remote_node_name),
      remote_protocol_version_(remote_protocol_version),
      transport_(std::move(transport)),
      link_memory_(std::move(link_memory)) {
  transport_->set_listener(this);
}

NodeLink::~NodeLink() {
  Deactivate();
}

RoutingId NodeLink::AllocateRoutingIds(size_t count) {
  return buffer().AllocateRoutingIds(count);
}

mem::Ref<RouterLink> NodeLink::AddRoute(RoutingId routing_id,
                                        size_t link_state_index,
                                        mem::Ref<Router> router) {
  absl::MutexLock lock(&mutex_);
  auto result = routes_.try_emplace(routing_id, router);
  ABSL_ASSERT(result.second);
  return mem::MakeRefCounted<RemoteRouterLink>(mem::WrapRefCounted(this),
                                               routing_id, link_state_index);
}

void NodeLink::Deactivate() {
  {
    absl::MutexLock lock(&mutex_);
    routes_.clear();
    if (!active_) {
      return;
    }

    active_ = false;
  }

  transport_->Deactivate();
}

void NodeLink::Transmit(absl::Span<const uint8_t> data,
                        absl::Span<os::Handle> handles) {
  transport_->TransmitMessage(DriverTransport::Message(data, handles));
}

mem::Ref<Router> NodeLink::GetRouter(RoutingId routing_id) {
  absl::MutexLock lock(&mutex_);
  auto it = routes_.find(routing_id);
  if (it == routes_.end()) {
    return nullptr;
  }
  return it->second;
}

IpczResult NodeLink::OnTransportMessage(
    const DriverTransport::Message& message) {
  const auto& header =
      *reinterpret_cast<const internal::MessageHeader*>(message.data.data());

  switch (header.message_id) {
    case msg::AcceptParcel::kId:
      if (OnAcceptParcel(message)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;

    case msg::SideClosed::kId: {
      msg::SideClosed side_closed;
      if (side_closed.Deserialize(message) && OnSideClosed(side_closed)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    default:
      break;
  }

  return IPCZ_RESULT_OK;
}

void NodeLink::OnTransportError() {}

bool NodeLink::OnAcceptParcel(const DriverTransport::Message& message) {
  if (message.data.size() < sizeof(msg::AcceptParcel)) {
    return false;
  }
  const auto& accept =
      *reinterpret_cast<const msg::AcceptParcel*>(message.data.data());
  const uint32_t num_bytes = accept.num_bytes;
  const uint32_t num_portals = accept.num_portals;
  const uint32_t num_os_handles = accept.num_os_handles;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&accept + 1);
  const auto* descriptors =
      reinterpret_cast<const PortalDescriptor*>(bytes + num_bytes);

  if (message.handles.size() != num_os_handles) {
    return false;
  }

  Parcel::PortalVector portals(num_portals);
  for (size_t i = 0; i < num_portals; ++i) {
    const PortalDescriptor& descriptor = descriptors[i];
    auto new_router = Router::Deserialize(descriptor);
    auto new_router_link = AddRoute(descriptor.new_routing_id,
                                    descriptor.new_routing_id, new_router);
    if (descriptor.route_is_peer) {
      new_router->SetPeer(std::move(new_router_link));
    } else {
      new_router->SetPredecessor(std::move(new_router_link));
    }
    portals[i] = mem::MakeRefCounted<Portal>(node_, std::move(new_router));
  }

  std::vector<os::Handle> os_handles(num_os_handles);
  for (size_t i = 0; i < num_os_handles; ++i) {
    os_handles[i] = std::move(message.handles[i]);
  }

  Parcel parcel(accept.sequence_number);
  parcel.SetData(std::vector<uint8_t>(bytes, bytes + num_bytes));
  parcel.SetPortals(std::move(portals));
  parcel.SetOSHandles(std::move(os_handles));
  mem::Ref<Router> receiver = GetRouter(accept.routing_id);
  if (!receiver) {
    return true;
  }

  return receiver->AcceptParcelFrom(*this, accept.routing_id, parcel);
}

bool NodeLink::OnSideClosed(const msg::SideClosed& side_closed) {
  mem::Ref<Router> receiver = GetRouter(side_closed.params.routing_id);
  if (!receiver) {
    return true;
  }

  receiver->AcceptRouteClosure(side_closed.params.side,
                               side_closed.params.sequence_length);
  return true;
}

}  // namespace core
}  // namespace ipcz
