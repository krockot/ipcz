// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstddef>
#include <cstring>
#include <memory>
#include <tuple>

#include "ipcz/ipcz.h"
#include "ipcz/node.h"
#include "ipcz/portal.h"
#include "ipcz/router.h"
#include "ipcz/trap.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/handle_util.h"
#include "util/os_process.h"
#include "util/ref_counted.h"

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

IpczResult Close(IpczHandle handle, uint32_t flags, const void* options) {
  Ref<APIObject> doomed_object(RefCounted::kAdoptExistingRef,
                               ToPtr<APIObject>(handle));
  if (!doomed_object) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return doomed_object->Close();
}

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

  if (!driver->Close || !driver->Serialize || !driver->Deserialize ||
      !driver->CreateTransports || !driver->ActivateTransport ||
      !driver->DeactivateTransport || !driver->Transmit ||
      !driver->AllocateSharedMemory || !driver->GetSharedMemoryInfo ||
      !driver->DuplicateSharedMemory || !driver->MapSharedMemory) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  auto node_ptr = MakeRefCounted<Node>((flags & IPCZ_CREATE_NODE_AS_BROKER) != 0
                                           ? Node::Type::kBroker
                                           : Node::Type::kNormal,
                                       *driver, driver_node);
  *node = ToHandle(node_ptr.release());
  return IPCZ_RESULT_OK;
}

IpczResult ConnectNode(IpczHandle node_handle,
                       IpczDriverHandle driver_transport,
                       const IpczOSProcessHandle* target_process,
                       uint32_t num_initial_portals,
                       IpczConnectNodeFlags flags,
                       const void* options,
                       IpczHandle* initial_portals) {
  Node* node = APIObject::Get<Node>(node_handle);
  if (!node || driver_transport == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (target_process && target_process->size < sizeof(IpczOSProcessHandle)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (num_initial_portals == 0 || !initial_portals) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (num_initial_portals > 16) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  OSProcess process;
  if (target_process) {
    if (target_process->size < sizeof(IpczOSProcessHandle)) {
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }
    process = OSProcess::FromIpczOSProcessHandle(*target_process);
  }

  return node->ConnectNode(
      driver_transport, std::move(process), flags,
      absl::Span<IpczHandle>(initial_portals, num_initial_portals));
}

IpczResult OpenPortals(IpczHandle node_handle,
                       uint32_t flags,
                       const void* options,
                       IpczHandle* portal0,
                       IpczHandle* portal1) {
  Node* node = APIObject::Get<Node>(node_handle);
  if (!node || !portal0 || !portal1) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  auto portals = node->OpenPortals();
  *portal0 = ToHandle(portals.first.release());
  *portal1 = ToHandle(portals.second.release());
  return IPCZ_RESULT_OK;
}

IpczResult MergePortals(IpczHandle portal0,
                        IpczHandle portal1,
                        uint32_t flags,
                        const void* options) {
  Portal* first = APIObject::Get<Portal>(portal0);
  Portal* second = APIObject::Get<Portal>(portal1);
  if (!first || !second) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  Ref<Portal> one(RefCounted::kAdoptExistingRef, first);
  Ref<Portal> two(RefCounted::kAdoptExistingRef, second);
  IpczResult result = one->Merge(*two);
  if (result != IPCZ_RESULT_OK) {
    one.release();
    two.release();
    return result;
  }

  return IPCZ_RESULT_OK;
}

IpczResult QueryPortalStatus(IpczHandle portal_handle,
                             uint32_t flags,
                             const void* options,
                             IpczPortalStatus* status) {
  Portal* portal = APIObject::Get<Portal>(portal_handle);
  if (!portal) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (!status || status->size < sizeof(IpczPortalStatus)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return portal->QueryStatus(*status);
}

IpczResult Put(IpczHandle portal_handle,
               const void* data,
               uint32_t num_bytes,
               const IpczHandle* handles,
               uint32_t num_handles,
               const IpczOSHandle* os_handles,
               uint32_t num_os_handles,
               uint32_t flags,
               const IpczPutOptions* options) {
  Portal* portal = APIObject::Get<Portal>(portal_handle);
  if (!portal) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (options && options->size < sizeof(IpczPutOptions)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_bytes > 0 && !data) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_handles > 0 && !handles) {
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
  return portal->Put(absl::MakeSpan(bytes, num_bytes),
                     absl::MakeSpan(handles, num_handles),
                     absl::MakeSpan(os_handles, num_os_handles), limits);
}

IpczResult BeginPut(IpczHandle portal_handle,
                    IpczBeginPutFlags flags,
                    const IpczBeginPutOptions* options,
                    uint32_t* num_bytes,
                    void** data) {
  Portal* portal = APIObject::Get<Portal>(portal_handle);
  if (!portal) {
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
  return portal->BeginPut(flags, limits, *num_bytes, data);
}

IpczResult EndPut(IpczHandle portal_handle,
                  uint32_t num_bytes_produced,
                  const IpczHandle* handles,
                  uint32_t num_handles,
                  const IpczOSHandle* os_handles,
                  uint32_t num_os_handles,
                  IpczEndPutFlags flags,
                  const void* options) {
  Portal* portal = APIObject::Get<Portal>(portal_handle);
  if (!portal) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_handles > 0 && !handles) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_os_handles > 0 && !os_handles) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (flags & IPCZ_END_PUT_ABORT) {
    return portal->AbortPut();
  }

  return portal->CommitPut(num_bytes_produced,
                           absl::MakeSpan(handles, num_handles),
                           absl::MakeSpan(os_handles, num_os_handles));
}

IpczResult Get(IpczHandle portal_handle,
               uint32_t flags,
               const void* options,
               void* data,
               uint32_t* num_bytes,
               IpczHandle* handles,
               uint32_t* num_handles,
               IpczOSHandle* os_handles,
               uint32_t* num_os_handles) {
  Portal* portal = APIObject::Get<Portal>(portal_handle);
  if (!portal) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_bytes && *num_bytes > 0 && !data) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_handles && *num_handles > 0 && !handles) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_os_handles && *num_os_handles > 0 && !os_handles) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return portal->Get(data, num_bytes, handles, num_handles, os_handles,
                     num_os_handles);
}

IpczResult BeginGet(IpczHandle portal_handle,
                    uint32_t flags,
                    const void* options,
                    const void** data,
                    uint32_t* num_bytes,
                    uint32_t* num_handles,
                    uint32_t* num_os_handles) {
  Portal* portal = APIObject::Get<Portal>(portal_handle);
  if (!portal) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return portal->BeginGet(data, num_bytes, num_handles, num_os_handles);
}

IpczResult EndGet(IpczHandle portal_handle,
                  uint32_t num_bytes_consumed,
                  IpczEndGetFlags flags,
                  const void* options,
                  IpczHandle* handles,
                  uint32_t* num_handles,
                  struct IpczOSHandle* os_handles,
                  uint32_t* num_os_handles) {
  Portal* portal = APIObject::Get<Portal>(portal_handle);
  if (!portal) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_handles && *num_handles > 0 && !handles) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_os_handles && *num_os_handles && !os_handles) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (flags & IPCZ_END_GET_ABORT) {
    return portal->AbortGet();
  }

  return portal->CommitGet(num_bytes_consumed, handles, num_handles, os_handles,
                           num_os_handles);
}

IpczResult CreateTrap(IpczHandle portal_handle,
                      const IpczTrapConditions* conditions,
                      IpczTrapEventHandler handler,
                      uint64_t context,
                      uint32_t flags,
                      const void* options,
                      IpczHandle* trap) {
  Portal* portal = APIObject::Get<Portal>(portal_handle);
  if (!portal || !trap || !handler) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (!conditions || conditions->size < sizeof(IpczTrapConditions)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return portal->CreateTrap(*conditions, handler, context, *trap);
}

IpczResult ArmTrap(IpczHandle trap_handle,
                   uint32_t flags,
                   const void* options,
                   IpczTrapConditionFlags* satisfied_condition_flags,
                   IpczPortalStatus* status) {
  Trap* trap = APIObject::Get<Trap>(trap_handle);
  if (!trap) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (status && status->size < sizeof(IpczPortalStatus)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return trap->Arm(satisfied_condition_flags, status);
}

constexpr IpczAPI kCurrentAPI = {
    sizeof(kCurrentAPI),
    Close,
    CreateNode,
    ConnectNode,
    OpenPortals,
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
};

constexpr size_t kVersion0APISize =
    offsetof(IpczAPI, ArmTrap) + sizeof(kCurrentAPI.ArmTrap);

MAYBE_EXPORT IpczResult IpczGetAPI(IpczAPI* api) {
  if (!api || api->size < kVersion0APISize) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  memcpy(api, &kCurrentAPI, kVersion0APISize);
  return IPCZ_RESULT_OK;
}

}  // extern "C"
