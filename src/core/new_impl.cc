#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/driver_transport.h"
#include "core/incoming_parcel_queue.h"
#include "core/new_impl.h"
#include "core/node_link_state.h"
#include "core/node_messages.h"
#include "core/outgoing_parcel_queue.h"
#include "core/parcel.h"
#include "core/portal_link_state.h"
#include "core/routing_mode.h"
#include "core/sequence_number.h"
#include "core/side.h"
#include "debug/log.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/handle_util.h"

namespace ipcz {
namespace core {

namespace {

struct IPCZ_ALIGN(8) AcceptParcelHeader {
  internal::MessageHeader message_header;
  RoutingId routing_id;
  SequenceNumber sequence_number;
  uint32_t num_bytes;
  uint32_t num_portals;
  uint32_t num_os_handles;
};

struct IPCZ_ALIGN(16) IntroduceNodeHeader {
  internal::MessageHeader message_header;
  bool known : 1;
  NodeName name;
  uint32_t num_transport_bytes;
  uint32_t num_transport_os_handles;
};

}  // namespace

using RouterLinkState = PortalLinkState;

class ZNodeLink;

struct IPCZ_ALIGN(8) ZPortalDescriptor {
  bool route_is_peer : 1;
  RoutingId new_routing_id;
};

// A memory buffer shared by both ends of a NodeLink.
class NodeLinkBuffer {
 public:
  NodeLinkBuffer() = default;
  ~NodeLinkBuffer() = default;

  static void Init(void* memory, uint32_t num_initial_portals) {
    NodeLinkBuffer& buffer = *(new (memory) NodeLinkBuffer());
    buffer.link_state_.AllocateRoutingIds(num_initial_portals);
    buffer.next_router_link_index_ = num_initial_portals;
  }

  NodeLinkState& node_link_state() { return link_state_; }

  RouterLinkState& router_link_state(uint32_t index) {
    return router_link_states_[index];
  }

  RoutingId AllocateRoutingIds(size_t count) {
    return link_state_.AllocateRoutingIds(count);
  }

 private:
  NodeLinkState link_state_;

  // TODO: this is quite a hack - implement a pool of NodeLinkBuffers and
  // allocate RouterLinkStates dynamically
  std::atomic<uint32_t> next_router_link_index_{0};
  std::array<RouterLinkState, 2048> router_link_states_;
};

class RouterLink : public mem::RefCounted {
 public:
  virtual RouterLinkState& GetLinkState() = 0;
  virtual bool IsLocalLinkTo(Router& router) = 0;
  virtual bool IsRemoteLinkTo(ZNodeLink& node_link, RoutingId routing_id) = 0;
  virtual bool WouldParcelExceedLimits(size_t data_size,
                                       const IpczPutLimits& limits) = 0;
  virtual void AcceptParcel(Parcel& parcel) = 0;
  virtual void AcceptRouteClosure(Side side,
                                  SequenceNumber sequence_length) = 0;

 protected:
  ~RouterLink() override = default;
};

class LocalRouterLinkState : public mem::RefCounted {
 public:
  LocalRouterLinkState(mem::Ref<Router> left, mem::Ref<Router> right)
      : sides_(std::move(left), std::move(right)) {}

  RouterLinkState& state() { return state_; }
  const mem::Ref<Router>& side(Side side) const { return sides_[side]; }

 private:
  ~LocalRouterLinkState() override = default;

  RouterLinkState state_;
  const TwoSided<mem::Ref<Router>> sides_;
};

// Local link between routers. A LocalRouterLink can only ever serve as a peer
// link. Predecessor and successor links are only created when extending a route
// across a ZNodeLink, which means they are always RemoteRouterLinks.
class LocalRouterLink : public RouterLink {
 public:
  LocalRouterLink(Side side, mem::Ref<LocalRouterLinkState> state)
      : side_(side), state_(std::move(state)) {}

  RouterLinkState& GetLinkState() override {
    return state_->state();
  }

  bool IsLocalLinkTo(Router& router) override;
  bool IsRemoteLinkTo(ZNodeLink& node_link, RoutingId routing_id) override {
    return false;
  }
  bool WouldParcelExceedLimits(size_t data_size,
                               const IpczPutLimits& limits) override;
  void AcceptParcel(Parcel& parcel) override;
  void AcceptRouteClosure(Side side, SequenceNumber sequence_length) override;

 private:
  ~LocalRouterLink() override = default;

  const Side side_;
  const mem::Ref<LocalRouterLinkState> state_;
};

class RemoteRouterLink : public RouterLink {
 public:
  RemoteRouterLink(mem::Ref<ZNodeLink> node_link,
                   RoutingId routing_id,
                   uint32_t link_state_index);

  const mem::Ref<ZNodeLink>& node_link() const { return node_link_; }
  RoutingId routing_id() const { return routing_id_; }

  RouterLinkState& GetLinkState() override;
  bool IsLocalLinkTo(Router& router) override;
  bool IsRemoteLinkTo(ZNodeLink& node_link, RoutingId routing_id) override {
    return node_link_.get() == &node_link && routing_id_ == routing_id;
  }
  bool WouldParcelExceedLimits(size_t data_size,
                               const IpczPutLimits& limits) override;
  void AcceptParcel(Parcel& parcel) override;
  void AcceptRouteClosure(Side side, SequenceNumber sequence_length) override;

 private:
  ~RemoteRouterLink() override;

  const mem::Ref<ZNodeLink> node_link_;
  const RoutingId routing_id_;
  const uint32_t link_state_index_;
};

class Router : public mem::RefCounted {
 public:
  explicit Router(Side side) : side_(side) {}

  void SetObserver(mem::Ref<RouterObserver> observer) {
    absl::MutexLock lock(&mutex_);
    observer_ = std::move(observer);
  }

  mem::Ref<RouterObserver> GetObserver() {
    absl::MutexLock lock(&mutex_);
    return observer_;
  }

  // Returns true iff this Router's peer link is a LocalRouterLink and its local
  // peer is `other`.
  bool HasLocalPeer(const mem::Ref<Router>& router) {
    absl::MutexLock lock(&mutex_);
    return peer_ && peer_->IsLocalLinkTo(*router);
  }

  // Returns true iff sending a parcel of `data_size` towards the other side of
  // the route may exceed the specified `limits` on the receiving end.
  bool WouldOutgoingParcelExceedLimits(size_t data_size,
                                       const IpczPutLimits& limits) {
    mem::Ref<RouterLink> link;
    {
      absl::MutexLock lock(&mutex_);
      if (routing_mode_ == RoutingMode::kBuffering) {
        return buffered_parcels_.size() < limits.max_queued_parcels &&
               buffered_parcels_.data_size() <=
                   limits.max_queued_bytes + data_size;
      }

      ABSL_ASSERT(routing_mode_ == RoutingMode::kActive);
      link = peer_ ? peer_ : predecessor_;
    }

    ABSL_ASSERT(link);
    return link->WouldParcelExceedLimits(data_size, limits);
  }

  // Returns true iff accepting an incoming parcel of `data_size` would cause
  // this router's incoming parcel queue to exceed limits specified by `limits`.
  bool WouldIncomingParcelExceedLimits(size_t data_size,
                                       const IpczPutLimits& limits) {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(routing_mode_ == RoutingMode::kActive);
    return incoming_parcels_.GetNumAvailableBytes() < limits.max_queued_bytes &&
           incoming_parcels_.GetNumAvailableParcels() <
               limits.max_queued_parcels;
  }

  // Attempts to send an outgoing parcel originating from this Router. Called
  // only as a direct result of a Put() call on the router's owning portal.
  IpczResult SendOutgoingParcel(absl::Span<const uint8_t> data,
                                Parcel::PortalVector& portals,
                                std::vector<os::Handle>& os_handles) {
    Parcel parcel(
        outgoing_sequence_length_.fetch_add(1, std::memory_order_relaxed));
    parcel.SetData(std::vector<uint8_t>(data.begin(), data.end()));
    parcel.SetPortals(std::move(portals));
    parcel.SetOSHandles(std::move(os_handles));

    mem::Ref<RouterLink> link;
    {
      absl::MutexLock lock(&mutex_);
      if (routing_mode_ == RoutingMode::kBuffering) {
        buffered_parcels_.push(std::move(parcel));
        return IPCZ_RESULT_OK;
      }

      ABSL_ASSERT(routing_mode_ == RoutingMode::kActive);
      link = peer_ ? peer_ : predecessor_;
    }

    link->AcceptParcel(parcel);
    return IPCZ_RESULT_OK;
  }

  // Closes this side of the Router's own route. Only called on a Router to
  // which a Portal is currently attached, and only by that Portal.
  void CloseRoute() {
    mem::Ref<RouterLink> someone_who_cares;
    SequenceNumber sequence_length;
    {
      RoutingMode routing_mode = RoutingMode::kClosed;
      absl::MutexLock lock(&mutex_);
      std::swap(routing_mode, routing_mode_);
      ABSL_ASSERT(routing_mode == RoutingMode::kBuffering ||
                  routing_mode == RoutingMode::kActive);
      if (routing_mode == RoutingMode::kActive) {
        someone_who_cares = peer_ ? peer_ : predecessor_;
        sequence_length = outgoing_sequence_length_;
      }
    }

    if (someone_who_cares) {
      someone_who_cares->AcceptRouteClosure(side_, sequence_length);
    }
  }

  // Activates a buffering router.
  void Activate(mem::Ref<RouterLink> link) {
    OutgoingParcelQueue parcels;
    {
      absl::MutexLock lock(&mutex_);
      ABSL_ASSERT(routing_mode_ == RoutingMode::kBuffering);
      routing_mode_ = RoutingMode::kActive;
      peer_ = link;
      parcels = std::move(buffered_parcels_);
    }

    for (Parcel& parcel : parcels.TakeParcels()) {
      link->AcceptParcel(parcel);
    }
  }

  // Accepts a parcel routed here from `link` via `routing_id`, which is
  // determined to be either an incoming or outgoing parcel based on the source
  // and routing mode.
  bool AcceptParcelFrom(ZNodeLink& link,
                        RoutingId routing_id,
                        Parcel& parcel,
                        TrapEventDispatcher& dispatcher) {
    bool is_incoming = false;
    {
      absl::MutexLock lock(&mutex_);
      if (peer_ && peer_->IsRemoteLinkTo(link, routing_id)) {
        is_incoming = true;
      } else if (predecessor_ &&
                 predecessor_->IsRemoteLinkTo(link, routing_id)) {
        is_incoming = true;
      } else if (!successor_ || !successor_->IsRemoteLinkTo(link, routing_id)) {
        return false;
      }
    }

    if (is_incoming) {
      return AcceptIncomingParcel(parcel);
    }

    return AcceptOutgoingParcel(parcel);
  }

  // Accepts an incoming parcel routed here from some other Router. What happens
  // to the parcel depends on the Router's current RoutingMode and established
  // links to other Routers.
  bool AcceptIncomingParcel(Parcel& parcel) {
    mem::Ref<RouterObserver> observer;
    uint32_t num_parcels;
    uint32_t num_bytes;
    {
      absl::MutexLock lock(&mutex_);
      observer = observer_;
      if (!incoming_parcels_.Push(std::move(parcel))) {
        return false;
      }

      num_parcels = incoming_parcels_.GetNumAvailableParcels();
      num_bytes = incoming_parcels_.GetNumAvailableBytes();
    }

    if (observer) {
      observer->OnIncomingParcel(num_parcels, num_bytes);
    }
    return true;
  }

  // Accepts an outgoing parcel router here from some other Router. What happens
  // to the parcel depends on the Router's current RoutingMode and its
  // established links to other Routers.
  bool AcceptOutgoingParcel(Parcel& parcel) { return false; }

  // Accepts notification that one `side` of this route has been closed.
  // Depending on current routing mode and established links, this notification
  // may be propagated elsewhere by this Router.
  void AcceptRouteClosure(Side side, SequenceNumber sequence_length) {
    if (side == side_) {
      // assert?
      return;
    }

    bool is_route_dead = false;
    mem::Ref<RouterObserver> observer = GetObserver();
    if (!observer) {
      return;
    } else {
      absl::MutexLock lock(&mutex_);
      incoming_parcels_.SetPeerSequenceLength(sequence_length);
      is_route_dead = incoming_parcels_.IsDead();
    }

    observer->OnPeerClosed(is_route_dead);
  }

  IpczResult GetNextIncomingParcel(void* data,
                                   uint32_t* num_bytes,
                                   IpczHandle* portals,
                                   uint32_t* num_portals,
                                   IpczOSHandle* os_handles,
                                   uint32_t* num_os_handles) {
    absl::MutexLock lock(&mutex_);
    if (!incoming_parcels_.HasNextParcel()) {
      if (incoming_parcels_.IsDead()) {
        return IPCZ_RESULT_NOT_FOUND;
      }
      return IPCZ_RESULT_UNAVAILABLE;
    }

    Parcel& p = incoming_parcels_.NextParcel();
    const uint32_t data_capacity = num_bytes ? *num_bytes : 0;
    const uint32_t data_size = static_cast<uint32_t>(p.data_view().size());
    const uint32_t portals_capacity = num_portals ? *num_portals : 0;
    const uint32_t portals_size =
        static_cast<uint32_t>(p.portals_view().size());
    const uint32_t os_handles_capacity = num_os_handles ? *num_os_handles : 0;
    const uint32_t os_handles_size =
        static_cast<uint32_t>(p.os_handles_view().size());
    if (num_bytes) {
      *num_bytes = data_size;
    }
    if (num_portals) {
      *num_portals = portals_size;
    }
    if (num_os_handles) {
      *num_os_handles = os_handles_size;
    }
    if (data_capacity < data_size || portals_capacity < portals_size ||
        os_handles_capacity < os_handles_size) {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
    }

    Parcel parcel;
    incoming_parcels_.Pop(parcel);
    memcpy(data, parcel.data_view().data(), parcel.data_view().size());
    parcel.Consume(portals, os_handles);
    return IPCZ_RESULT_OK;
  }

 private:
  ~Router() override = default;

  const Side side_;
  std::atomic<SequenceNumber> outgoing_sequence_length_{0};

  absl::Mutex mutex_;
  mem::Ref<RouterObserver> observer_ ABSL_GUARDED_BY(mutex_);
  RoutingMode routing_mode_ ABSL_GUARDED_BY(mutex_) = RoutingMode::kBuffering;
  mem::Ref<RouterLink> peer_ ABSL_GUARDED_BY(mutex_);
  mem::Ref<RouterLink> successor_ ABSL_GUARDED_BY(mutex_);
  mem::Ref<RouterLink> predecessor_ ABSL_GUARDED_BY(mutex_);
  OutgoingParcelQueue buffered_parcels_ ABSL_GUARDED_BY(mutex_);
  IncomingParcelQueue incoming_parcels_ ABSL_GUARDED_BY(mutex_);
};

bool LocalRouterLink::WouldParcelExceedLimits(size_t data_size,
                                              const IpczPutLimits& limits) {
  return state_->side(Opposite(side_))
      ->WouldIncomingParcelExceedLimits(data_size, limits);
}

bool LocalRouterLink::IsLocalLinkTo(Router& router) {
  return state_->side(Opposite(side_)).get() == &router;
}

void LocalRouterLink::AcceptParcel(Parcel& parcel) {
  state_->side(Opposite(side_))->AcceptIncomingParcel(parcel);
}

void LocalRouterLink::AcceptRouteClosure(Side side,
                                         SequenceNumber sequence_length) {
  state_->side(Opposite(side_))->AcceptRouteClosure(side, sequence_length);
}

class ZNodeLink : public mem::RefCounted, private DriverTransport::Listener {
 public:
  ZNodeLink(mem::Ref<ZNode> node,
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

  const NodeName& remote_node_name() const { return remote_node_name_; }
  uint32_t remote_protocol_version() const { return remote_protocol_version_; }
  const mem::Ref<DriverTransport>& transport() const { return transport_; }
  NodeLinkBuffer& buffer() const { return *link_memory_.As<NodeLinkBuffer>(); }

  RoutingId AllocateRoutingIds(size_t count) {
    return buffer().AllocateRoutingIds(count);
  }

  mem::Ref<RouterLink> AddRoute(RoutingId routing_id,
                                size_t link_state_index,
                                mem::Ref<Router> router) {
    absl::MutexLock lock(&mutex_);
    auto result = routes_.try_emplace(routing_id, router);
    ABSL_ASSERT(result.second);
    return mem::MakeRefCounted<RemoteRouterLink>(mem::WrapRefCounted(this),
                                                 routing_id, link_state_index);
  }

  void Deactivate() { transport_->Deactivate(); }

  template <typename T>
  void Transmit(T& message) {
    transport_->Transmit(message);
  }

  void Transmit(absl::Span<const uint8_t> data,
                absl::Span<os::Handle> handles) {
    transport_->TransmitMessage(DriverTransport::Message(data, handles));
  }

 private:
  ~ZNodeLink() override { Deactivate(); }

  mem::Ref<Router> GetRouter(RoutingId routing_id) {
    absl::MutexLock lock(&mutex_);
    auto it = routes_.find(routing_id);
    if (it == routes_.end()) {
      return nullptr;
    }
    return it->second;
  }

  // DriverTransport::Listener:
  IpczResult OnTransportMessage(
      const DriverTransport::Message& message) override {
    const auto& header =
        *reinterpret_cast<const internal::MessageHeader*>(message.data.data());

    switch (header.message_id) {
      case msg::kAcceptParcelId:
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

  void OnTransportError() override {}

  bool OnAcceptParcel(const DriverTransport::Message& message) {
    if (message.data.size() < sizeof(AcceptParcelHeader)) {
      return false;
    }

    const auto& header =
        *reinterpret_cast<const AcceptParcelHeader*>(message.data.data());
    const uint32_t num_bytes = header.num_bytes;
    const uint32_t num_portals = header.num_portals;
    const uint32_t num_os_handles = header.num_os_handles;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&header + 1);
    const auto* descriptors =
        reinterpret_cast<const ZPortalDescriptor*>(bytes + num_bytes);

    if (message.handles.size() != num_os_handles) {
      return false;
    }

    Parcel::PortalVector portals(num_portals);
    for (size_t i = 0; i < num_portals; ++i) {
      // TODO
    }
    (void)descriptors;

    std::vector<os::Handle> os_handles(num_os_handles);
    for (size_t i = 0; i < num_os_handles; ++i) {
      os_handles[i] = std::move(message.handles[i]);
    }

    TrapEventDispatcher dispatcher;
    Parcel parcel(header.sequence_number);
    parcel.SetData(std::vector<uint8_t>(bytes, bytes + num_bytes));
    parcel.SetPortals(std::move(portals));
    parcel.SetOSHandles(std::move(os_handles));
    mem::Ref<Router> receiver = GetRouter(header.routing_id);
    return !receiver || receiver->AcceptParcelFrom(*this, header.routing_id,
                                                   parcel, dispatcher);
  }

  bool OnSideClosed(const msg::SideClosed& side_closed) {
    mem::Ref<Router> receiver = GetRouter(side_closed.params.routing_id);
    if (!receiver) {
      return false;
    }

    receiver->AcceptRouteClosure(side_closed.params.side,
                                 side_closed.params.sequence_length);
    return true;
  }

  const mem::Ref<ZNode> node_;
  const NodeName remote_node_name_;
  const uint32_t remote_protocol_version_;
  const mem::Ref<DriverTransport> transport_;
  const os::Memory::Mapping link_memory_;

  absl::Mutex mutex_;
  absl::flat_hash_map<RoutingId, mem::Ref<Router>> routes_
      ABSL_GUARDED_BY(mutex_);
};

RemoteRouterLink::RemoteRouterLink(mem::Ref<ZNodeLink> node_link,
                                   RoutingId routing_id,
                                   uint32_t link_state_index)
    : node_link_(std::move(node_link)),
      routing_id_(routing_id),
      link_state_index_(link_state_index) {}

RemoteRouterLink::~RemoteRouterLink() = default;

RouterLinkState& RemoteRouterLink::GetLinkState() {
  return node_link_->buffer().router_link_state(link_state_index_);
}

bool RemoteRouterLink::IsLocalLinkTo(Router& router) {
  return false;
}

bool RemoteRouterLink::WouldParcelExceedLimits(size_t data_size,
                                               const IpczPutLimits& limits) {
  // TODO
  return false;
}

void RemoteRouterLink::AcceptParcel(Parcel& parcel) {
  absl::InlinedVector<uint8_t, 256> serialized_data;
  const size_t num_portals = parcel.portals_view().size();
  const size_t num_os_handles = parcel.os_handles_view().size();
  const size_t serialized_size =
      sizeof(AcceptParcelHeader) + parcel.data_view().size() +
      num_portals * sizeof(ZPortalDescriptor) +
      num_os_handles * sizeof(internal::OSHandleData);
  serialized_data.resize(serialized_size);

  auto& header = *reinterpret_cast<AcceptParcelHeader*>(serialized_data.data());
  header.message_header.size = sizeof(header.message_header);
  header.message_header.message_id = msg::kAcceptParcelId;
  header.routing_id = routing_id_;
  header.sequence_number = parcel.sequence_number();
  header.num_bytes = parcel.data_view().size();
  header.num_portals = static_cast<uint32_t>(num_portals);
  header.num_os_handles = static_cast<uint32_t>(num_os_handles);
  auto* data = reinterpret_cast<uint8_t*>(&header + 1);
  memcpy(data, parcel.data_view().data(), parcel.data_view().size());
  auto* descriptors =
      reinterpret_cast<ZPortalDescriptor*>(data + header.num_bytes);

  const absl::Span<mem::Ref<ZPortal>> portals = parcel.portals_view();
  const RoutingId first_routing_id =
      node_link()->AllocateRoutingIds(num_portals);
  std::vector<os::Handle> os_handles;
  std::vector<mem::Ref<RouterLink>> new_links(num_portals);
  os_handles.reserve(num_os_handles);
  for (size_t i = 0; i < num_portals; ++i) {
    const RoutingId routing_id = first_routing_id + i;
    RouterLinkState& state =
        node_link()->buffer().router_link_state(routing_id);
    RouterLinkState::Initialize(&state);
    descriptors[i].new_routing_id = routing_id;
    portals[i] = portals[i]->Serialize(descriptors[i]);
    new_links[i] =
        node_link()->AddRoute(routing_id, routing_id, portals[i]->router());
  }

  node_link()->Transmit(absl::MakeSpan(serialized_data),
                        parcel.os_handles_view());
}

void RemoteRouterLink::AcceptRouteClosure(Side side,
                                          SequenceNumber sequence_length) {
  msg::SideClosed side_closed;
  side_closed.params.routing_id = routing_id_;
  side_closed.params.side = side;
  side_closed.params.sequence_length = sequence_length;
  node_link()->Transmit(side_closed);
}

ZNode::ZNode(Type type, const IpczDriver& driver, IpczDriverHandle driver_node)
    : type_(type), driver_(driver), driver_node_(driver_node) {}

ZNode::~ZNode() = default;

void ZNode::ShutDown() {
}

class ConnectListener : public DriverTransport::Listener,
                        public mem::RefCounted {
 public:
  ConnectListener(mem::Ref<ZNode> node,
                  mem::Ref<DriverTransport> transport,
                  std::vector<mem::Ref<ZPortal>> waiting_portals,
                  os::Memory::Mapping link_buffer_mapping)
      : node_(std::move(node)),
        transport_(std::move(transport)),
        waiting_portals_(std::move(waiting_portals)),
        link_buffer_mapping_(std::move(link_buffer_mapping)) {}

  using ConnectHandler = std::function<void(mem::Ref<ZNodeLink> link)>;

  void Listen(ConnectHandler handler) { handler_ = std::move(handler); }

  // DriverTransport::Listener:
  IpczResult OnTransportMessage(
      const DriverTransport::Message& message) override {
    const auto& header =
        *reinterpret_cast<const internal::MessageHeader*>(message.data.data());
    if (header.message_id != msg::Connect::kId) {
      return IPCZ_RESULT_UNIMPLEMENTED;
    }

    msg::Connect connect;
    if (!connect.Deserialize(message)) {
      handler_ = nullptr;
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    if (!link_buffer_mapping_.is_valid()) {
      os::Memory memory(std::move(connect.handles.link_state_memory),
                        sizeof(NodeLinkBuffer));
      link_buffer_mapping_ = memory.Map();
    }

    auto node_link = mem::MakeRefCounted<ZNodeLink>(
        node_, connect.params.name, connect.params.protocol_version, transport_,
        std::move(link_buffer_mapping_));

    if (!handler_) {
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    TrapEventDispatcher dispatcher;
    ConnectHandler handler = std::move(handler_);
    handler(node_link);
    for (size_t i = 0; i < waiting_portals_.size(); ++i) {
      const mem::Ref<Router>& router = waiting_portals_[i]->router();
      router->Activate(
          node_link->AddRoute(static_cast<RoutingId>(i), i, router));
    }
    return IPCZ_RESULT_OK;
  }

  void OnTransportError() override {
    if (handler_) {
      handler_(nullptr);
      handler_ = nullptr;
    }
  }

 private:
  ~ConnectListener() override = default;

  const mem::Ref<ZNode> node_;
  const mem::Ref<DriverTransport> transport_;
  const std::vector<mem::Ref<ZPortal>> waiting_portals_;
  os::Memory::Mapping link_buffer_mapping_;
  ConnectHandler handler_;
};

IpczResult ZNode::ConnectNode(IpczDriverHandle driver_transport,
                              Type remote_node_type,
                              os::Process remote_process,
                              absl::Span<IpczHandle> initial_portals) {
  std::vector<mem::Ref<ZPortal>> buffering_portals(initial_portals.size());
  const Side side = type_ == Type::kBroker ? Side::kLeft : Side::kRight;
  for (size_t i = 0; i < initial_portals.size(); ++i) {
    auto portal = mem::MakeRefCounted<ZPortal>(
        mem::WrapRefCounted(this), mem::MakeRefCounted<Router>(side));
    buffering_portals[i] = portal;
    initial_portals[i] = ToHandle(portal.release());
  }

  const uint32_t num_portals = static_cast<uint32_t>(initial_portals.size());
  os::Memory::Mapping link_state_mapping;
  msg::Connect connect;
  connect.params.protocol_version = msg::kProtocolVersion;
  connect.params.name = name_;
  connect.params.num_initial_portals = num_portals;
  if (type_ == Type::kBroker) {
    os::Memory link_state_memory(sizeof(NodeLinkBuffer));
    link_state_mapping = link_state_memory.Map();
    NodeLinkBuffer::Init(link_state_mapping.base(), num_portals);
    connect.handles.link_state_memory = link_state_memory.TakeHandle();
  }

  auto transport = mem::MakeRefCounted<DriverTransport>(driver_,
                                                        driver_transport);
  auto listener = mem::MakeRefCounted<ConnectListener>(
      mem::WrapRefCounted(this), transport, std::move(buffering_portals),
      std::move(link_state_mapping));
  listener->Listen(
      [node = mem::WrapRefCounted(this), listener](mem::Ref<ZNodeLink> link) {
        const NodeName name = link->remote_node_name();
        node->AddLink(name, std::move(link));
      });

  transport->set_listener(listener.get());
  transport->Activate();
  transport->Transmit(connect);

  // TODO
  (void)driver_node_;

  return IPCZ_RESULT_OK;
}

std::pair<mem::Ref<ZPortal>, mem::Ref<ZPortal>> ZNode::OpenPortals() {
  return ZPortal::CreatePair(mem::WrapRefCounted(this));
}

bool ZNode::AddLink(const NodeName& remote_node_name,
                    mem::Ref<ZNodeLink> link) {
  absl::MutexLock lock(&mutex_);
  auto result = node_links_.try_emplace(remote_node_name, std::move(link));
  return result.second;
}

ZPortal::ZPortal(mem::Ref<ZNode> node, mem::Ref<Router> router)
    : node_(std::move(node)), router_(std::move(router)) {
  router_->SetObserver(mem::WrapRefCounted(this));
}

ZPortal::~ZPortal() {
  router_->SetObserver(nullptr);
}

// static
std::pair<mem::Ref<ZPortal>, mem::Ref<ZPortal>> ZPortal::CreatePair(
    mem::Ref<ZNode> node) {
  auto left_router = mem::MakeRefCounted<Router>(Side::kLeft);
  auto right_router = mem::MakeRefCounted<Router>(Side::kRight);
  auto link_state = mem::MakeRefCounted<LocalRouterLinkState>(left_router,
                                                              right_router);
  auto left = mem::MakeRefCounted<ZPortal>(node, std::move(left_router));
  auto right = mem::MakeRefCounted<ZPortal>(std::move(node),
                                            std::move(right_router));
  TrapEventDispatcher dispatcher;
  left->router()->Activate(
      mem::MakeRefCounted<LocalRouterLink>(Side::kLeft, link_state));
  right->router()->Activate(
      mem::MakeRefCounted<LocalRouterLink>(Side::kRight, link_state));
  return {std::move(left), std::move(right)};
}

mem::Ref<ZPortal> ZPortal::Serialize(ZPortalDescriptor& descriptor) {
  return mem::WrapRefCounted(this);
}

IpczResult ZPortal::Close() {
  router_->CloseRoute();
  return IPCZ_RESULT_OK;
}

IpczResult ZPortal::QueryStatus(IpczPortalStatus& status) {
  absl::MutexLock lock(&mutex_);
  uint32_t size = std::min(status.size, status_.size);
  memcpy(&status, &status_, size);
  status.size = size;
  return IPCZ_RESULT_OK;
}

bool ValidateAndAcquirePortalsForTransitFrom(
    ZPortal& sender,
    absl::Span<const IpczHandle> handles,
    Parcel::PortalVector& portals) {
  portals.resize(handles.size());
  for (size_t i = 0; i < handles.size(); ++i) {
    auto portal = mem::WrapRefCounted(ToPtr<ZPortal>(handles[i]));
    if (&sender == portal.get() ||
        sender.router()->HasLocalPeer(portal->router())) {
      return false;
    }
    portals[i] = std::move(portal);
  }
  return true;
}

IpczResult ZPortal::Put(absl::Span<const uint8_t> data,
                        absl::Span<const IpczHandle> portal_handles,
                        absl::Span<const IpczOSHandle> os_handles,
                        const IpczPutLimits* limits) {
  Parcel::PortalVector portals;
  if (!ValidateAndAcquirePortalsForTransitFrom(*this, portal_handles,
                                               portals)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (limits &&
      router_->WouldOutgoingParcelExceedLimits(data.size(), *limits)) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  std::vector<os::Handle> handles(os_handles.size());
  for (size_t i = 0; i < os_handles.size(); ++i) {
    handles[i] = os::Handle::FromIpczOSHandle(os_handles[i]);
  }

  IpczResult result = router_->SendOutgoingParcel(data, portals, handles);
  if (result != IPCZ_RESULT_OK) {
    for (os::Handle& handle : handles) {
      (void)handle.release();
    }
    return result;
  }

  absl::MutexLock lock(&mutex_);
  status_.num_remote_parcels += 1;
  status_.num_remote_bytes += static_cast<uint32_t>(data.size());
  return IPCZ_RESULT_OK;
}

IpczResult ZPortal::BeginPut(IpczBeginPutFlags flags,
                             const IpczPutLimits* limits,
                             uint32_t& num_data_bytes,
                             void** data) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult ZPortal::CommitPut(uint32_t num_data_bytes_produced,
                              absl::Span<const IpczHandle> portals,
                              absl::Span<const IpczOSHandle> os_handles) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult ZPortal::AbortPut() {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult ZPortal::Get(void* data,
                        uint32_t* num_data_bytes,
                        IpczHandle* portals,
                        uint32_t* num_portals,
                        IpczOSHandle* os_handles,
                        uint32_t* num_os_handles) {
  IpczResult result = router_->GetNextIncomingParcel(
      data, num_data_bytes, portals, num_portals, os_handles, num_os_handles);
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  absl::MutexLock lock(&mutex_);
  status_.num_local_parcels -= 1;
  if (num_data_bytes) {
    status_.num_local_bytes -= *num_data_bytes;
  }
  return IPCZ_RESULT_OK;
}

IpczResult ZPortal::BeginGet(const void** data,
                             uint32_t* num_data_bytes,
                             uint32_t* num_portals,
                             uint32_t* num_os_handles) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult ZPortal::CommitGet(uint32_t num_data_bytes_consumed,
                              IpczHandle* portals,
                              uint32_t* num_portals,
                              IpczOSHandle* os_handles,
                              uint32_t* num_os_handles) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult ZPortal::AbortGet() {
  return IPCZ_RESULT_UNIMPLEMENTED;
}


IpczResult ZPortal::CreateTrap(const IpczTrapConditions& conditions,
                               IpczTrapEventHandler handler,
                               uintptr_t context,
                               IpczHandle& trap) {
  auto new_trap = std::make_unique<Trap>(conditions, handler, context);
  trap = ToHandle(new_trap.get());

  absl::MutexLock lock(&mutex_);
  return traps_.Add(std::move(new_trap));
}

IpczResult ZPortal::ArmTrap(IpczHandle trap,
                            IpczTrapConditionFlags* satisfied_condition_flags,
                            IpczPortalStatus* status) {
  IpczTrapConditionFlags flags = 0;
  absl::MutexLock lock(&mutex_);
  IpczResult result = ToRef<Trap>(trap).Arm(status_, flags);
  if (result == IPCZ_RESULT_OK) {
    return result;
  }

  if (satisfied_condition_flags) {
    *satisfied_condition_flags = flags;
  }

  if (status) {
    size_t size = std::min(status_.size, status->size);
    memcpy(status, &status_, size);
    status->size = size;
  }
  return result;
}

IpczResult ZPortal::DestroyTrap(IpczHandle trap) {
  absl::MutexLock lock(&mutex_);
  return traps_.Remove(ToRef<Trap>(trap));
}

void ZPortal::OnPeerClosed(bool is_route_dead) {
  TrapEventDispatcher dispatcher;
  absl::MutexLock lock(&mutex_);
  status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
  if (is_route_dead) {
    status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
  }
  traps_.MaybeNotify(dispatcher, status_);
}

void ZPortal::OnIncomingParcel(uint32_t num_available_parcels,
                               uint32_t num_available_bytes) {
  TrapEventDispatcher dispatcher;
  absl::MutexLock lock(&mutex_);
  status_.num_local_parcels = num_available_parcels;
  status_.num_remote_parcels = num_available_bytes;
  traps_.MaybeNotify(dispatcher, status_);
}

TrapEventDispatcher dispatcher;
}  // namespace core
}  // mamespace ipcz
