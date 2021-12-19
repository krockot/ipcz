// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstddef>
#include <cstring>
#include <memory>
#include <tuple>

#include "core/node.h"
#include "core/portal.h"
#include "core/router.h"
#include "core/trap.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/process.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/handle_util.h"

#if defined(IPCZ_SHARED_LIBRARY)
#if defined(WIN32)
#define MAYBE_EXPORT __declspec(dllexport)
#else
#define MAYBE_EXPORT __attribute__((visibility("default")))
#endif
#else
#define MAYBE_EXPORT
#endif

using namespace ipcz;

extern "C" {

IpczResult CreateNode(const IpczDriver* driver,
                      IpczDriverHandle driver_node,
                      IpczCreateNodeFlags flags,
                      const void* options,
                      IpczHandle* node) {
  if (!node || !driver) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (driver->size < sizeof(IpczDriver)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (!driver->CreateTransports || !driver->DestroyTransport ||
      !driver->SerializeTransport || !driver->DeserializeTransport ||
      !driver->ActivateTransport || !driver->Transmit) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  auto node_ptr = mem::MakeRefCounted<core::Node>(
      (flags & IPCZ_CREATE_NODE_AS_BROKER) != 0 ? core::Node::Type::kBroker
                                                : core::Node::Type::kNormal,
      *driver, driver_node);
  *node = ToHandle(node_ptr.release());
  return IPCZ_RESULT_OK;
}

IpczResult DestroyNode(IpczHandle node, uint32_t flags, const void* options) {
  if (node == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  mem::Ref<core::Node> doomed_node(mem::RefCounted::kAdoptExistingRef,
                                   ToPtr<core::Node>(node));
  doomed_node->ShutDown();
  doomed_node.reset();
  return IPCZ_RESULT_OK;
}

IpczResult ConnectNode(IpczHandle node_handle,
                       IpczDriverHandle driver_transport,
                       const IpczOSProcessHandle* target_process,
                       uint32_t num_initial_portals,
                       IpczConnectNodeFlags flags,
                       const void* options,
                       IpczHandle* initial_portals) {
  if (node_handle == IPCZ_INVALID_HANDLE ||
      driver_transport == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (target_process && target_process->size < sizeof(IpczOSProcessHandle)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (num_initial_portals == 0 || !initial_portals) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  os::Process process;
  if (target_process) {
    if (target_process->size < sizeof(IpczOSProcessHandle)) {
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }
    process = os::Process::FromIpczOSProcessHandle(*target_process);
  }

  core::Node& node = ToRef<core::Node>(node_handle);
  return node.ConnectNode(
      driver_transport, std::move(process), flags,
      absl::Span<IpczHandle>(initial_portals, num_initial_portals));
}

IpczResult OpenPortals(IpczHandle node,
                       uint32_t flags,
                       const void* options,
                       IpczHandle* portal0,
                       IpczHandle* portal1) {
  if (node == IPCZ_INVALID_HANDLE || !portal0 || !portal1) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  auto portals = ToRef<core::Node>(node).OpenPortals();
  *portal0 = ToHandle(portals.first.release());
  *portal1 = ToHandle(portals.second.release());
  return IPCZ_RESULT_OK;
}

IpczResult MergePortals(IpczHandle first,
                        IpczHandle second,
                        uint32_t flags,
                        const void* options) {
  if (first == IPCZ_INVALID_HANDLE || second == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  mem::Ref<core::Portal> one(mem::RefCounted::kAdoptExistingRef,
                             ToPtr<core::Portal>(first));
  mem::Ref<core::Portal> two(mem::RefCounted::kAdoptExistingRef,
                             ToPtr<core::Portal>(second));
  IpczResult result = one->Merge(*two);
  if (result != IPCZ_RESULT_OK) {
    one.release();
    two.release();
    return result;
  }

  return IPCZ_RESULT_OK;
}

IpczResult ClosePortal(IpczHandle portal, uint32_t flags, const void* options) {
  if (portal == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  // The Portal may outlive this call, but it's no longer reachable through any
  // ipcz API calls.
  mem::Ref<core::Portal> released_portal(mem::RefCounted::kAdoptExistingRef,
                                         ToPtr<core::Portal>(portal));
  released_portal->Close();
  return IPCZ_RESULT_OK;
}

IpczResult QueryPortalStatus(IpczHandle portal,
                             uint32_t flags,
                             const void* options,
                             IpczPortalStatus* status) {
  if (portal == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (!status || status->size < sizeof(IpczPortalStatus)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return ToRef<core::Portal>(portal).QueryStatus(*status);
}

IpczResult Put(IpczHandle portal,
               const void* data,
               uint32_t num_bytes,
               const IpczHandle* portals,
               uint32_t num_portals,
               const IpczOSHandle* os_handles,
               uint32_t num_os_handles,
               uint32_t flags,
               const IpczPutOptions* options) {
  if (portal == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (options && options->size < sizeof(IpczPutOptions)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_bytes > 0 && !data) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_portals > 0 && !portals) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_os_handles > 0 && !os_handles) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  const IpczPutLimits* limits = options ? options->limits : nullptr;
  if (limits && limits->size < sizeof(IpczPutLimits)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  const auto* bytes = static_cast<const uint8_t*>(data);
  return ToRef<core::Portal>(portal).Put(
      absl::MakeSpan(bytes, num_bytes), absl::MakeSpan(portals, num_portals),
      absl::MakeSpan(os_handles, num_os_handles), limits);
}

IpczResult BeginPut(IpczHandle portal,
                    IpczBeginPutFlags flags,
                    const IpczBeginPutOptions* options,
                    uint32_t* num_bytes,
                    void** data) {
  if (portal == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_bytes && *num_bytes > 0 && !data) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (options && options->size < sizeof(IpczBeginPutOptions)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  const IpczPutLimits* limits = options ? options->limits : nullptr;
  if (limits && limits->size < sizeof(IpczPutLimits)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  uint32_t dummy_num_bytes = 0;
  if (!num_bytes) {
    num_bytes = &dummy_num_bytes;
  }
  return ToRef<core::Portal>(portal).BeginPut(flags, limits, *num_bytes, data);
}

IpczResult EndPut(IpczHandle portal,
                  uint32_t num_bytes_produced,
                  const IpczHandle* portals,
                  uint32_t num_portals,
                  const IpczOSHandle* os_handles,
                  uint32_t num_os_handles,
                  IpczEndPutFlags flags,
                  const void* options) {
  if (portal == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_portals > 0 && !portals) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_os_handles > 0 && !os_handles) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (flags & IPCZ_END_PUT_ABORT) {
    return ToRef<core::Portal>(portal).AbortPut();
  }

  return ToRef<core::Portal>(portal).CommitPut(
      num_bytes_produced, absl::MakeSpan(portals, num_portals),
      absl::MakeSpan(os_handles, num_os_handles));
}

IpczResult Get(IpczHandle portal,
               uint32_t flags,
               const void* options,
               void* data,
               uint32_t* num_bytes,
               IpczHandle* portals,
               uint32_t* num_portals,
               IpczOSHandle* os_handles,
               uint32_t* num_os_handles) {
  if (portal == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_bytes && *num_bytes > 0 && !data) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_portals && *num_portals > 0 && !portals) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_os_handles && *num_os_handles > 0 && !os_handles) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return ToRef<core::Portal>(portal).Get(data, num_bytes, portals, num_portals,
                                         os_handles, num_os_handles);
}

IpczResult BeginGet(IpczHandle portal,
                    uint32_t flags,
                    const void* options,
                    const void** data,
                    uint32_t* num_bytes,
                    uint32_t* num_portals,
                    uint32_t* num_os_handles) {
  if (portal == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return ToRef<core::Portal>(portal).BeginGet(data, num_bytes, num_portals,
                                              num_os_handles);
}

IpczResult EndGet(IpczHandle portal,
                  uint32_t num_bytes_consumed,
                  IpczEndGetFlags flags,
                  const void* options,
                  IpczHandle* portals,
                  uint32_t* num_portals,
                  struct IpczOSHandle* os_handles,
                  uint32_t* num_os_handles) {
  if (portal == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_portals && *num_portals > 0 && !portals) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_os_handles && *num_os_handles && !os_handles) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (flags & IPCZ_END_GET_ABORT) {
    return ToRef<core::Portal>(portal).AbortGet();
  }

  return ToRef<core::Portal>(portal).CommitGet(
      num_bytes_consumed, portals, num_portals, os_handles, num_os_handles);
}

IpczResult CreateTrap(IpczHandle portal,
                      const IpczTrapConditions* conditions,
                      IpczTrapEventHandler handler,
                      uint64_t context,
                      uint32_t flags,
                      const void* options,
                      IpczHandle* trap) {
  if (portal == IPCZ_INVALID_HANDLE || !trap || !handler) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (!conditions || conditions->size < sizeof(IpczTrapConditions)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return ToRef<core::Portal>(portal).CreateTrap(*conditions, handler, context,
                                                *trap);
}

IpczResult ArmTrap(IpczHandle trap,
                   uint32_t flags,
                   const void* options,
                   IpczTrapConditionFlags* satisfied_condition_flags,
                   IpczPortalStatus* status) {
  if (trap == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (status && status->size < sizeof(IpczPortalStatus)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return ToRef<core::Trap>(trap).Arm(satisfied_condition_flags, status);
}

IpczResult DestroyTrap(IpczHandle trap,
                       IpczDestroyTrapFlags flags,
                       const void* options) {
  if (trap == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  mem::Ref<core::Trap> doomed_trap(mem::RefCounted::kAdoptExistingRef,
                                   ToPtr<core::Trap>(trap));
  doomed_trap->portal()->router()->RemoveTrap(*doomed_trap);
  doomed_trap->Disable(flags);
  return IPCZ_RESULT_OK;
}

constexpr IpczAPI kCurrentAPI = {
    sizeof(kCurrentAPI),
    CreateNode,
    DestroyNode,
    ConnectNode,
    OpenPortals,
    ClosePortal,
    MergePortals,
    QueryPortalStatus,
    Put,
    BeginPut,
    EndPut,
    Get,
    BeginGet,
    EndGet,
    CreateTrap,
    ArmTrap,
    DestroyTrap,
};

constexpr size_t kVersion0APISize =
    offsetof(IpczAPI, DestroyTrap) + sizeof(kCurrentAPI.DestroyTrap);

MAYBE_EXPORT IpczResult IpczGetAPI(IpczAPI* api) {
  if (!api || api->size < kVersion0APISize) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  memcpy(api, &kCurrentAPI, kVersion0APISize);
  return IPCZ_RESULT_OK;
}

}  // extern "C"
