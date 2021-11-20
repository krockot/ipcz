#ifndef NEW_IMPL_H_
#define NEW_IMPL_H_

#include <atomic>
#include <cstdint>
#include <utility>

#include "core/node_name.h"
#include "core/parcel.h"
#include "core/side.h"
#include "core/trap.h"
#include "core/trap_event_dispatcher.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/process.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class Router;
class ZNodeLink;
class ZPortal;

class ZNode : public mem::RefCounted {
 public:
  enum class Type {
    kBroker,
    kNormal,
  };

  ZNode(Type type, const IpczDriver& driver, IpczDriverHandle driver_node);

  const NodeName& name() const { return name_; }
  Type type() const { return type_; }
  const IpczDriver& driver() const { return driver_; }

  void ShutDown();
  IpczResult ConnectNode(IpczDriverHandle driver_transport,
                         Type remote_node_type,
                         os::Process remote_process,
                         absl::Span<IpczHandle> initial_portals);
  std::pair<mem::Ref<ZPortal>, mem::Ref<ZPortal>> OpenPortals();

 private:
  ~ZNode() override;

  bool AddLink(const NodeName& remote_node_name, mem::Ref<ZNodeLink> link);

  const NodeName name_{NodeName::kRandom};
  const Type type_;
  const IpczDriver driver_;
  const IpczDriverHandle driver_node_;

  absl::Mutex mutex_;
  absl::flat_hash_map<NodeName, mem::Ref<ZNodeLink>> node_links_
      ABSL_GUARDED_BY(mutex_);
};

class RouterObserver : public mem::RefCounted {
 public:
  virtual void OnPeerClosed(bool is_route_dead) = 0;
  virtual void OnIncomingParcel(uint32_t num_available_parcels,
                                uint32_t num_avialable_bytes) = 0;

 protected:
  ~RouterObserver() override = default;
};

struct ZPortalDescriptor;

class ZPortal : public RouterObserver {
 public:
  ZPortal(mem::Ref<ZNode> node, mem::Ref<Router> router);

  const mem::Ref<ZNode>& node() const { return node_; }
  const mem::Ref<Router>& router() const { return router_; }

  static std::pair<mem::Ref<ZPortal>, mem::Ref<ZPortal>> CreatePair(
      mem::Ref<ZNode> node);

  mem::Ref<ZPortal> Serialize(ZPortalDescriptor& descriptor);

  // ipcz portal API implementation:
  IpczResult Close();
  IpczResult QueryStatus(IpczPortalStatus& status);

  IpczResult Put(absl::Span<const uint8_t> data,
                 absl::Span<const IpczHandle> portals,
                 absl::Span<const IpczOSHandle> os_handles,
                 const IpczPutLimits* limits);
  IpczResult BeginPut(IpczBeginPutFlags flags,
                      const IpczPutLimits* limits,
                      uint32_t& num_data_bytes,
                      void** data);
  IpczResult CommitPut(uint32_t num_data_bytes_produced,
                       absl::Span<const IpczHandle> portals,
                       absl::Span<const IpczOSHandle> os_handles);
  IpczResult AbortPut();

  IpczResult Get(void* data,
                 uint32_t* num_data_bytes,
                 IpczHandle* portals,
                 uint32_t* num_portals,
                 IpczOSHandle* os_handles,
                 uint32_t* num_os_handles);
  IpczResult BeginGet(const void** data,
                      uint32_t* num_data_bytes,
                      uint32_t* num_portals,
                      uint32_t* num_os_handles);
  IpczResult CommitGet(uint32_t num_data_bytes_consumed,
                       IpczHandle* portals,
                       uint32_t* num_portals,
                       IpczOSHandle* os_handles,
                       uint32_t* num_os_handles);
  IpczResult AbortGet();

  IpczResult CreateTrap(const IpczTrapConditions& conditions,
                        IpczTrapEventHandler handler,
                        uintptr_t context,
                        IpczHandle& trap);
  IpczResult ArmTrap(IpczHandle trap,
                     IpczTrapConditionFlags* satisfied_condition_flags,
                     IpczPortalStatus* status);
  IpczResult DestroyTrap(IpczHandle trap);

 private:
  ~ZPortal() override;

  // RouterObserver:
  void OnPeerClosed(bool is_route_dead) override;
  void OnIncomingParcel(uint32_t num_available_parcels,
                        uint32_t num_avialable_bytes) override;

  const mem::Ref<ZNode> node_;
  const mem::Ref<Router> router_;

  absl::Mutex mutex_;
  IpczPortalStatus status_ = {sizeof(status_)};
  TrapSet traps_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif
