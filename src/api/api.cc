// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstddef>
#include <cstring>
#include <memory>
#include <tuple>

#include "build/build_config.h"
#include "core/monitor.h"
#include "core/name.h"
#include "core/node.h"
#include "core/portal.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/channel.h"
#include "os/process.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/types/span.h"

#if defined(IPCZ_SHARED_LIBRARY)
#if defined(WIN32)
#define MAYBE_EXPORT __declspec(dllexport)
#else
#define MAYBE_EXPORT __attribute__((visibility("default")))
#endif
#else
#define MAYBE_EXPORT
#endif

namespace {

template <typename T>
IpczHandle ToHandle(const T* ptr) {
  return static_cast<IpczHandle>(reinterpret_cast<uintptr_t>(ptr));
}

ipcz::mem::Ref<ipcz::core::Node>& ToNode(IpczHandle node) {
  return *reinterpret_cast<ipcz::mem::Ref<ipcz::core::Node>*>(
      static_cast<uintptr_t>(node));
}

ipcz::core::Portal& ToPortal(IpczHandle portal) {
  return *reinterpret_cast<ipcz::core::Portal*>(static_cast<uintptr_t>(portal));
}

ipcz::core::Monitor& ToMonitor(IpczHandle monitor) {
  return *reinterpret_cast<ipcz::core::Monitor*>(
      static_cast<uintptr_t>(monitor));
}

}  // namespace

extern "C" {

IpczResult CreateNode(IpczCreateNodeFlags flags,
                      const void* options,
                      IpczHandle* node) {
  if (!node) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  auto node_ptr = std::make_unique<ipcz::mem::Ref<ipcz::core::Node>>(
      ipcz::mem::MakeRefCounted<ipcz::core::Node>());
  *node = ToHandle(node_ptr.release());
  return IPCZ_RESULT_OK;
}

IpczResult DestroyNode(IpczHandle node, uint32_t flags, const void* options) {
  if (node == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  std::unique_ptr<ipcz::mem::Ref<ipcz::core::Node>> doomed_node(&ToNode(node));
  return IPCZ_RESULT_OK;
}

IpczResult OpenPortals(IpczHandle node,
                       uint32_t flags,
                       const void* options,
                       IpczHandle* portal0,
                       IpczHandle* portal1) {
  if (node == IPCZ_INVALID_HANDLE || !portal0 || !portal1) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  ipcz::core::Portal::Pair portals = ToNode(node)->OpenPortals();
  *portal0 = ToHandle(portals.first.release());
  *portal1 = ToHandle(portals.second.release());
  return IPCZ_RESULT_OK;
}

IpczResult OpenRemotePortal(IpczHandle node,
                            const IpczOSTransport* transport,
                            const IpczOSProcessHandle* target_process,
                            uint32_t flags,
                            const void* options,
                            IpczHandle* portal) {
  if (node == IPCZ_INVALID_HANDLE || !portal) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (!transport || transport->size < sizeof(IpczOSTransport)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  ipcz::os::Process process;
  if (target_process) {
    if (target_process->size < sizeof(IpczOSProcessHandle)) {
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }
    process = ipcz::os::Process::FromIpczOSProcessHandle(*target_process);
  }

#if defined(OS_WIN)
  if (!process.is_valid()) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
#endif

  ipcz::os::Channel channel =
      ipcz::os::Channel::FromIpczOSTransport(*transport);
  if (!channel.is_valid()) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  std::unique_ptr<ipcz::core::Portal> new_portal;
  IpczResult result = ToNode(node)->OpenRemotePortal(
      std::move(channel), std::move(process), new_portal);
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  *portal = ToHandle(new_portal.release());
  return IPCZ_RESULT_OK;
}

IpczResult AcceptRemotePortal(IpczHandle node,
                              const IpczOSTransport* transport,
                              uint32_t flags,
                              const void* options,
                              IpczHandle* portal) {
  if (node == IPCZ_INVALID_HANDLE || !portal) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (!transport || transport->size < sizeof(IpczOSTransport)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  ipcz::os::Channel channel =
      ipcz::os::Channel::FromIpczOSTransport(*transport);
  std::unique_ptr<ipcz::core::Portal> new_portal;
  IpczResult result =
      ToNode(node)->AcceptRemotePortal(std::move(channel), new_portal);
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  *portal = ToHandle(new_portal.release());
  return IPCZ_RESULT_OK;
}

IpczResult ClosePortal(IpczHandle portal, uint32_t flags, const void* options) {
  if (portal == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  std::unique_ptr<ipcz::core::Portal> doomed_portal(&ToPortal(portal));
  doomed_portal->Close();
  return IPCZ_RESULT_OK;
}

IpczResult QueryPortalStatus(IpczHandle portal,
                             IpczPortalStatusFieldFlags field_flags,
                             uint32_t flags,
                             const void* options,
                             IpczPortalStatus* status) {
  if (portal == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (!status || status->size < sizeof(IpczPortalStatus)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return ToPortal(portal).QueryStatus(field_flags, *status);
}

IpczResult Put(IpczHandle portal,
               const void* data,
               uint32_t num_data_bytes,
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
  if (num_data_bytes > 0 && !data) {
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
  return ToPortal(portal).Put(absl::MakeSpan(bytes, num_data_bytes),
                              absl::MakeSpan(portals, num_portals),
                              absl::MakeSpan(os_handles, num_os_handles),
                              limits);
}

IpczResult BeginPut(IpczHandle portal,
                    IpczBeginPutFlags flags,
                    const IpczBeginPutOptions* options,
                    uint32_t* num_data_bytes,
                    void** data) {
  if (portal == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_data_bytes && *num_data_bytes > 0 && !data) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (options && options->size < sizeof(IpczBeginPutOptions)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  const IpczPutLimits* limits = options ? options->limits : nullptr;
  if (limits && limits->size < sizeof(IpczPutLimits)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return ToPortal(portal).BeginPut(flags, limits, *num_data_bytes, data);
}

IpczResult EndPut(IpczHandle portal,
                  uint32_t num_data_bytes_produced,
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
    return ToPortal(portal).AbortPut();
  }

  return ToPortal(portal).CommitPut(num_data_bytes_produced,
                                    absl::MakeSpan(portals, num_portals),
                                    absl::MakeSpan(os_handles, num_os_handles));
}

IpczResult Get(IpczHandle portal,
               uint32_t flags,
               const void* options,
               void* data,
               uint32_t* num_data_bytes,
               IpczHandle* portals,
               uint32_t* num_portals,
               IpczOSHandle* os_handles,
               uint32_t* num_os_handles) {
  if (portal == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_data_bytes && *num_data_bytes > 0 && !data) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_portals && *num_portals > 0 && !portals) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (num_os_handles && *num_os_handles > 0 && !os_handles) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return ToPortal(portal).Get(data, num_data_bytes, portals, num_portals,
                              os_handles, num_os_handles);
}

IpczResult BeginGet(IpczHandle portal,
                    uint32_t flags,
                    const void* options,
                    const void** data,
                    uint32_t* num_data_bytes,
                    uint32_t* num_portals,
                    uint32_t* num_os_handles) {
  if (portal == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return ToPortal(portal).BeginGet(data, num_data_bytes, num_portals,
                                   num_os_handles);
}

IpczResult EndGet(IpczHandle portal,
                  uint32_t num_data_bytes_consumed,
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
    return ToPortal(portal).AbortGet();
  }

  return ToPortal(portal).CommitGet(num_data_bytes_consumed, portals,
                                    num_portals, os_handles, num_os_handles);
}

IpczResult CreateMonitor(IpczHandle portal,
                         const IpczMonitorDescriptor* descriptor,
                         uint32_t flags,
                         const void* options,
                         IpczHandle* monitor) {
  if (portal == IPCZ_INVALID_HANDLE || !monitor) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (!descriptor || descriptor->size < sizeof(IpczMonitorDescriptor)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return ToPortal(portal).CreateMonitor(*descriptor, monitor);
}

IpczResult ActivateMonitor(IpczHandle monitor,
                           uint32_t flags,
                           const void* options,
                           IpczMonitorConditionFlags* conditions,
                           IpczPortalStatus* status) {
  if (monitor == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return ToMonitor(monitor).Activate(conditions, status);
}

IpczResult DestroyMonitor(IpczHandle monitor,
                          uint32_t flags,
                          const void* options) {
  if (monitor == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  std::unique_ptr<ipcz::core::Monitor> doomed_monitor(&ToMonitor(monitor));
  return IPCZ_RESULT_OK;
}

constexpr IpczAPI kCurrentAPI = {
    sizeof(kCurrentAPI),
    CreateNode,
    DestroyNode,
    OpenPortals,
    OpenRemotePortal,
    AcceptRemotePortal,
    ClosePortal,
    QueryPortalStatus,
    Put,
    BeginPut,
    EndPut,
    Get,
    BeginGet,
    EndGet,
    CreateMonitor,
    ActivateMonitor,
    DestroyMonitor,
};

constexpr size_t kVersion0APISize =
    offsetof(IpczAPI, DestroyMonitor) + sizeof(kCurrentAPI.DestroyMonitor);

MAYBE_EXPORT IpczResult IpczGetAPI(IpczAPI* api) {
  if (!api || api->size < kVersion0APISize) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  memcpy(api, &kCurrentAPI, kVersion0APISize);
  return IPCZ_RESULT_OK;
}

}  // extern "C"
