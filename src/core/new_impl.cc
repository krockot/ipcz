#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "build/build_config.h"
#include "core/new_impl.h"
#include "core/node_link_state.h"
#include "core/node_messages.h"
#include "core/parcel.h"
#include "core/portal_link_state.h"
#include "core/routing_mode.h"
#include "core/side.h"
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

#if defined(OS_WIN)
#define CDECL __cdecl
#else
#define CDECL
#endif

namespace ipcz {
namespace core {

class DriverTransport;

IpczResult CDECL NotifyTransport(IpczHandle transport,
                                 const uint8_t* data,
                                 uint32_t num_bytes,
                                 const struct IpczOSHandle* os_handles,
                                 uint32_t num_os_handles,
                                 IpczTransportActivityFlags flags,
                                 const void* options);

using RouterLinkState = PortalLinkState;

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

  uint32_t AllocateRouterLinkState() {
    return next_router_link_index_.fetch_add(1, std::memory_order_relaxed);
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
  virtual void AcceptParcel(Parcel& parcel) = 0;
  virtual void AcceptRouteClosure(Side side) = 0;

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

  void AcceptParcel(Parcel& parcel) override;
  void AcceptRouteClosure(Side side) override;

 private:
  ~LocalRouterLink() override = default;

  const Side side_;
  const mem::Ref<LocalRouterLinkState> state_;
};

class Router : public mem::RefCounted {
 public:
  explicit Router(Side side) : side_(side) {}

  // Transmits an outgoing Parcel originating from this Router. Only called on a
  // Router to which a Portal is currently attached, and only by that Portal.
  void SendParcel(Parcel& parcel) {
    parcel.set_sequence_number(
        next_outgoing_sequence_number_.fetch_add(1, std::memory_order_relaxed));

    // TODO
  }

  // Closes this side of the Router's own route. Only called on a Router to
  // which a Portal is currently attached, and only by that Portal.
  void CloseRoute() {
    mem::Ref<RouterLink> someone_who_cares;
    {
      RoutingMode routing_mode = RoutingMode::kClosed;
      absl::MutexLock lock(&mutex_);
      std::swap(routing_mode, routing_mode_);
      ABSL_ASSERT(routing_mode == RoutingMode::kBuffering ||
                  routing_mode == RoutingMode::kActive);
      if (routing_mode == RoutingMode::kActive) {
        someone_who_cares = peer_ ? peer_ : predecessor_;
      }
    }

    if (someone_who_cares) {
      someone_who_cares->AcceptRouteClosure(side_);
    }
  }

  // Accepts an incoming parcel routed here from some other Router. What happens
  // to the parcel depends on the Router's current RoutingMode and established
  // links to other Routers.
  void AcceptIncomingParcel(Parcel& parcel) {
    // TODO
  }

  // Accepts an outgoing parcel router here from some other Router. What happens
  // to the parcel depends on the Router's current RoutingMode and established
  // links to other Routers.
  void AcceptOutgoingParcel(Parcel& parcel) {
    // TODO
  }

  // Accepts notification that one `side` of this route has been closed.
  // Depending on current routing mode and established links, this notification
  // may be propagated elsewhere by this Router.
  void AcceptRouteClosure(Side side) {
    // TODO
  }

 private:
  ~Router() override = default;

  const Side side_;
  std::atomic<SequenceNumber> next_outgoing_sequence_number_{0};

  absl::Mutex mutex_;
  RoutingMode routing_mode_ = RoutingMode::kBuffering;
  mem::Ref<RouterLink> peer_ ABSL_GUARDED_BY(mutex_);
  mem::Ref<RouterLink> successor_ ABSL_GUARDED_BY(mutex_);
  mem::Ref<RouterLink> predecessor_ ABSL_GUARDED_BY(mutex_);
};

void LocalRouterLink::AcceptParcel(Parcel& parcel) {
  state_->side(Opposite(side_))->AcceptIncomingParcel(parcel);
}

void LocalRouterLink::AcceptRouteClosure(Side side) {
  // TODO
}

class DriverTransport : public mem::RefCounted {
 public:
  struct Descriptor {
    Descriptor() = default;
    Descriptor(Descriptor&&) = default;
    Descriptor& operator=(Descriptor&&) = default;
    ~Descriptor() = default;

    std::vector<uint8_t> data;
    std::vector<os::Handle> handles;
  };

  class Data : public absl::Span<const uint8_t> {
   public:
    Data() = default;
    Data(absl::Span<const uint8_t> data) : Span(data) {}
    Data(absl::Span<const char> str)
        : Span(reinterpret_cast<const uint8_t*>(str.data()), str.size()) {}

    template <size_t N>
    Data(const char str[N]) : Data(absl::MakeSpan(str)) {}

    absl::Span<const char> AsString() const {
      return absl::MakeSpan(reinterpret_cast<const char*>(data()), size());
    }
  };

  struct Message {
    Message(Data data) : data(data) {}
    Message(Data data, absl::Span<os::Handle> handles)
        : data(data), handles(handles) {}
    Message(const Message&) = default;
    Message& operator=(const Message&) = default;
    ~Message() = default;

    Data data;
    absl::Span<os::Handle> handles;
  };

  DriverTransport(const IpczDriver& driver, IpczDriverHandle driver_transport)
      : driver_(driver), driver_transport_(driver_transport) {}

  // Set the callback handling all incoming transmissions. Note that once the
  // transported is Activate()ed, this is only safe to call from within the
  // listener itself, as listener invocations are guaranteed to be mutually
  // exclusive.
  //
  // This avoids otherwise needless synchronization while still allowing for an
  // incoming transmission to change how subsequent transmissions are handled.
  using MessageListener = std::function<IpczResult(const Message&)>;
  void set_message_listener(MessageListener listener) {
    message_listener_ = std::move(listener);
  }

  // Similar to above but for handling a (one-time) error event reported by the
  // driver. Invocation of this listener implies no further invocations of
  // either the MessageListener or the ErrorListener.
  using ErrorListener = std::function<void()>;
  void set_error_listener(ErrorListener listener) {
    error_listener_ = std::move(listener);
  }

  IpczResult Activate() {
    // Acquire a self-reference, balanced in NotifyTransport() when the driver
    // invokes its activity handler with IPCZ_TRANSPORT_ACTIVITY_DEACTIVATED.
    IpczHandle handle = ToHandle(mem::WrapRefCounted(this).release());
    return driver_.ActivateTransport(driver_transport_, handle, NotifyTransport,
                                     IPCZ_NO_FLAGS, nullptr);
  }

  IpczResult Deactivate() {
    return driver_.DestroyTransport(driver_transport_, IPCZ_NO_FLAGS, nullptr);
  }

  IpczResult Transmit(const Message& message) {
    absl::InlinedVector<IpczOSHandle, 2> os_handles(message.handles.size());
    for (size_t i = 0; i < message.handles.size(); ++i) {
      os_handles[i].size = sizeof(os_handles[i]);
      bool ok = os::Handle::ToIpczOSHandle(std::move(message.handles[i]),
                                           &os_handles[i]);
      ABSL_ASSERT(ok);
    }

    return driver_.Transmit(
        driver_transport_, message.data.data(),
        static_cast<uint32_t>(message.data.size()), os_handles.data(),
        static_cast<uint32_t>(os_handles.size()), IPCZ_NO_FLAGS, nullptr);
  }

  IpczResult Notify(const Message& message) {
    ABSL_ASSERT(message_listener_);
    return message_listener_(message);
  }

  void NotifyError() {
    ABSL_ASSERT(error_listener_);
    error_listener_();
  }

 private:
  ~DriverTransport() override = default;

  const IpczDriver& driver_;
  const IpczDriverHandle driver_transport_;

  // A callback to invoke on each message notification from the driver. See the
  // note on set_listener() regarding synchronization.
  MessageListener message_listener_;

  // A callback to invoke when an unrecoverable error is reported by the driver.
  ErrorListener error_listener_;
};

class ZNodeLink : public mem::RefCounted {
 public:
  ZNodeLink() = default;

 private:
  ~ZNodeLink() override = default;
};

ZNode::ZNode(Type type, const IpczDriver& driver, IpczDriverHandle driver_node)
    : type_(type), driver_(driver), driver_node_(driver_node) {}

ZNode::~ZNode() = default;

void ZNode::ShutDown() {
}

IpczResult ZNode::ConnectNode(IpczDriverHandle driver_transport,
                              Type remote_node_type,
                              os::Process remote_process,
                              absl::Span<IpczHandle> initial_portals) {
  const Side side = type_ == Type::kBroker ? Side::kLeft : Side::kRight;
  for (IpczHandle& handle : initial_portals) {
    auto portal = mem::MakeRefCounted<ZPortal>(
        mem::WrapRefCounted(this), mem::MakeRefCounted<Router>(side));
    handle = ToHandle(portal.release());
  }

  const uint32_t num_portals = static_cast<uint32_t>(initial_portals.size());
  msg::Connect connect;
  connect.params.protocol_version = msg::kProtocolVersion;
  connect.params.name = name_;
  connect.params.num_initial_portals = num_portals;
  if (type_ == Type::kBroker) {
    os::Memory link_state_memory(sizeof(NodeLinkBuffer));
    os::Memory::Mapping link_state_mapping = link_state_memory.Map();
    NodeLinkBuffer::Init(link_state_mapping.base(), num_portals);
    connect.handles.link_state_memory = link_state_memory.TakeHandle();
  }

  auto transport = mem::MakeRefCounted<DriverTransport>(driver_,
                                                        driver_transport);
  transport->set_message_listener(
      [node=mem::WrapRefCounted(this)](
          const DriverTransport::Message& message) {
            return IPCZ_RESULT_OK;
          });

  // TODO
  (void)driver_node_;

  return IPCZ_RESULT_UNIMPLEMENTED;
}

std::pair<mem::Ref<ZPortal>, mem::Ref<ZPortal>> ZNode::OpenPortals() {
  return ZPortal::CreatePair(mem::WrapRefCounted(this));
}

ZPortal::ZPortal(mem::Ref<ZNode> node, mem::Ref<Router> router)
    : node_(std::move(node)), router_(std::move(router)) {}

ZPortal::~ZPortal() = default;

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
  return {std::move(left), std::move(right)};
}

IpczResult ZPortal::Close() {
  router_->CloseRoute();
  return IPCZ_RESULT_OK;
}

IpczResult ZPortal::QueryStatus(IpczPortalStatus& status) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult ZPortal::Put(absl::Span<const uint8_t> data,
                        absl::Span<const IpczHandle> portals,
                        absl::Span<const IpczOSHandle> os_handles,
                        const IpczPutLimits* limits) {
  return IPCZ_RESULT_UNIMPLEMENTED;
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
  return IPCZ_RESULT_UNIMPLEMENTED;
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
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult ZPortal::ArmTrap(IpczHandle trap,
                            IpczTrapConditionFlags* satisfied_condition_flags,
                            IpczPortalStatus* status) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult ZPortal::DestroyTrap(IpczHandle trap) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult CDECL NotifyTransport(IpczHandle transport,
                                 const uint8_t* data,
                                 uint32_t num_bytes,
                                 const struct IpczOSHandle* os_handles,
                                 uint32_t num_os_handles,
                                 IpczTransportActivityFlags flags,
                                 const void* options) {
  if (transport == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (flags & IPCZ_TRANSPORT_ACTIVITY_DEACTIVATED) {
    mem::Ref<DriverTransport> doomed_transport(
        mem::RefCounted::kAdoptExistingRef, ToPtr<DriverTransport>(transport));
    return IPCZ_RESULT_OK;
  }

  DriverTransport& t = ToRef<DriverTransport>(transport);
  if (flags & IPCZ_TRANSPORT_ACTIVITY_ERROR) {
    t.NotifyError();
    return IPCZ_RESULT_OK;
  }

  absl::InlinedVector<os::Handle, 2> handles;
  handles.resize(num_os_handles);
  for (size_t i = 0; i < num_os_handles; ++i) {
    handles[i] = os::Handle::FromIpczOSHandle(os_handles[i]);
  }

  return t.Notify(DriverTransport::Message(
      absl::Span<const uint8_t>(data, num_bytes), absl::MakeSpan(handles)));
}

}  // namespace core
}  // mamespace ipcz
