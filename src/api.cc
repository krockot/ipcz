// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstddef>
#include <cstring>
#include <memory>
#include <tuple>

#include "ipcz/box.h"
#include "ipcz/driver_object.h"
#include "ipcz/ipcz.h"
#include "ipcz/node.h"
#include "ipcz/portal.h"
#include "ipcz/router.h"
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

extern "C" {

IpczResult Close(IpczHandle handle, uint32_t flags, const void* options) {
  ipcz::Ref<ipcz::APIObject> doomed_object(
      ipcz::RefCounted::kAdoptExistingRef,
      ipcz::ToPtr<ipcz::APIObject>(handle));
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

  auto node_ptr = ipcz::MakeRefCounted<ipcz::Node>(
      (flags & IPCZ_CREATE_NODE_AS_BROKER) != 0 ? ipcz::Node::Type::kBroker
                                                : ipcz::Node::Type::kNormal,
      *driver, driver_node);
  *node = ipcz::ToHandle(node_ptr.release());
  return IPCZ_RESULT_OK;
}

IpczResult ConnectNode(IpczHandle node_handle,
                       IpczDriverHandle driver_transport,
                       const IpczOSProcessHandle* target_process,
                       uint32_t num_initial_portals,
                       IpczConnectNodeFlags flags,
                       const void* options,
                       IpczHandle* initial_portals) {
  ipcz::Node* node = ipcz::APIObject::Get<ipcz::Node>(node_handle);
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

  ipcz::OSProcess process;
  if (target_process) {
    if (target_process->size < sizeof(IpczOSProcessHandle)) {
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }
    process = ipcz::OSProcess::FromIpczOSProcessHandle(*target_process);
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
  ipcz::Node* node = ipcz::APIObject::Get<ipcz::Node>(node_handle);
  if (!node || !portal0 || !portal1) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  auto portals = node->OpenPortals();
  *portal0 = ipcz::ToHandle(portals.first.release());
  *portal1 = ipcz::ToHandle(portals.second.release());
  return IPCZ_RESULT_OK;
}

IpczResult MergePortals(IpczHandle portal0,
                        IpczHandle portal1,
                        uint32_t flags,
                        const void* options) {
  ipcz::Portal* first = ipcz::APIObject::Get<ipcz::Portal>(portal0);
  ipcz::Portal* second = ipcz::APIObject::Get<ipcz::Portal>(portal1);
  if (!first || !second) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  ipcz::Ref<ipcz::Portal> one(ipcz::RefCounted::kAdoptExistingRef, first);
  ipcz::Ref<ipcz::Portal> two(ipcz::RefCounted::kAdoptExistingRef, second);
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
  ipcz::Portal* portal = ipcz::APIObject::Get<ipcz::Portal>(portal_handle);
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
  ipcz::Portal* portal = ipcz::APIObject::Get<ipcz::Portal>(portal_handle);
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
  ipcz::Portal* portal = ipcz::APIObject::Get<ipcz::Portal>(portal_handle);
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
  ipcz::Portal* portal = ipcz::APIObject::Get<ipcz::Portal>(portal_handle);
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
               IpczGetFlags flags,
               const void* options,
               void* data,
               uint32_t* num_bytes,
               IpczHandle* handles,
               uint32_t* num_handles,
               IpczOSHandle* os_handles,
               uint32_t* num_os_handles) {
  ipcz::Portal* portal = ipcz::APIObject::Get<ipcz::Portal>(portal_handle);
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

  return portal->Get(flags, data, num_bytes, handles, num_handles, os_handles,
                     num_os_handles);
}

IpczResult BeginGet(IpczHandle portal_handle,
                    uint32_t flags,
                    const void* options,
                    const void** data,
                    uint32_t* num_bytes,
                    uint32_t* num_handles,
                    uint32_t* num_os_handles) {
  ipcz::Portal* portal = ipcz::APIObject::Get<ipcz::Portal>(portal_handle);
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
                  uint32_t num_handles,
                  struct IpczOSHandle* os_handles,
                  uint32_t num_os_handles) {
  ipcz::Portal* portal = ipcz::APIObject::Get<ipcz::Portal>(portal_handle);
  if (!portal) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_handles > 0 && !handles) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_os_handles > 0 && !os_handles) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (flags & IPCZ_END_GET_ABORT) {
    return portal->AbortGet();
  }

  return portal->CommitGet(num_bytes_consumed,
                           absl::MakeSpan(handles, num_handles),
                           absl::MakeSpan(os_handles, num_os_handles));
}

IpczResult Trap(IpczHandle portal_handle,
                const IpczTrapConditions* conditions,
                IpczTrapEventHandler handler,
                uint64_t context,
                uint32_t flags,
                const void* options,
                IpczTrapConditionFlags* satisfied_condition_flags,
                IpczPortalStatus* status) {
  ipcz::Portal* portal = ipcz::APIObject::Get<ipcz::Portal>(portal_handle);
  if (!portal || !handler || !conditions ||
      conditions->size < sizeof(*conditions)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (status && status->size < sizeof(*status)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return portal->router()->Trap(*conditions, handler, context,
                                satisfied_condition_flags, status);
}

IpczResult Box(IpczHandle node_handle,
               IpczDriverHandle driver_handle,
               uint32_t flags,
               const void* options,
               IpczHandle* handle) {
  ipcz::Node* node = ipcz::APIObject::Get<ipcz::Node>(node_handle);
  if (!node || driver_handle == IPCZ_INVALID_DRIVER_HANDLE || !handle) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  auto box = ipcz::MakeRefCounted<ipcz::Box>(
      ipcz::DriverObject(ipcz::WrapRefCounted(node), driver_handle));
  *handle = ipcz::ToHandle(box.release());
  return IPCZ_RESULT_OK;
}

IpczResult Unbox(IpczHandle handle,
                 IpczUnboxFlags flags,
                 const void* options,
                 IpczDriverHandle* driver_handle) {
  if (!driver_handle) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  ipcz::Ref<ipcz::Box> box =
      ipcz::APIObject::ReleaseHandleAs<ipcz::Box>(handle);
  if (!box) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (flags & IPCZ_UNBOX_PEEK) {
    *driver_handle = box->object().handle();
    std::ignore = box.release();
  } else {
    *driver_handle = box->object().release();
  }
  return IPCZ_RESULT_OK;
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
    Trap,
    Box,
    Unbox,
};

constexpr size_t kVersion0APISize =
    offsetof(IpczAPI, Unbox) + sizeof(kCurrentAPI.Unbox);

MAYBE_EXPORT IpczResult IpczGetAPI(IpczAPI* api) {
  if (!api || api->size < kVersion0APISize) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  memcpy(api, &kCurrentAPI, kVersion0APISize);
  return IPCZ_RESULT_OK;
}

}  // extern "C"
