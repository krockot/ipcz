// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "reference_drivers/single_process_reference_driver.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "build/build_config.h"
#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/handle_util.h"
#include "util/os_handle.h"
#include "util/ref_counted.h"

#if defined(OS_WIN)
#define IPCZ_CDECL __cdecl
#else
#define IPCZ_CDECL
#endif

namespace ipcz {
namespace reference_drivers {

namespace {

class TransportWrapper : public RefCounted {
 public:
  TransportWrapper(IpczHandle transport,
                   IpczTransportActivityHandler activity_handler)
      : transport_(transport), activity_handler_(activity_handler) {}

  IpczResult Notify(absl::Span<const uint8_t> data,
                    absl::Span<const IpczOSHandle> os_handles) {
    IpczResult result = activity_handler_(
        transport_, data.data(), static_cast<uint32_t>(data.size()),
        os_handles.data(), static_cast<uint32_t>(os_handles.size()),
        IPCZ_NO_FLAGS, nullptr);
    if (result != IPCZ_RESULT_OK && result != IPCZ_RESULT_UNIMPLEMENTED) {
      NotifyError();
    }
    return result;
  }

  IpczResult NotifyError() {
    return activity_handler_(transport_, nullptr, 0, nullptr, 0,
                             IPCZ_TRANSPORT_ACTIVITY_ERROR, nullptr);
  }

 private:
  ~TransportWrapper() override {
    activity_handler_(transport_, nullptr, 0, nullptr, 0,
                      IPCZ_TRANSPORT_ACTIVITY_DEACTIVATED, nullptr);
  }

  const IpczHandle transport_;
  const IpczTransportActivityHandler activity_handler_;
};

struct SavedMessage {
  std::vector<uint8_t> data;
  std::vector<OSHandle> handles;
};

class InProcessTransport : public RefCounted {
 public:
  InProcessTransport() = default;
  ~InProcessTransport() override = default;

  void SetPeer(Ref<InProcessTransport> peer) {
    ABSL_ASSERT(!peer ^ !peer_);
    peer_ = std::move(peer);
  }

  IpczResult Activate(IpczHandle transport,
                      IpczTransportActivityHandler activity_handler) {
    {
      absl::MutexLock lock(&mutex_);
      ABSL_ASSERT(!transport_);
      transport_ =
          MakeRefCounted<TransportWrapper>(transport, activity_handler);
    }

    peer_->OnPeerActivated();
    return IPCZ_RESULT_OK;
  }

  void Deactivate() {
    Ref<TransportWrapper> transport;
    {
      absl::MutexLock lock(&mutex_);
      ABSL_ASSERT(transport_);
      transport = std::move(transport_);
    }
  }

  IpczResult Transmit(absl::Span<const uint8_t> data,
                      absl::Span<const IpczOSHandle> os_handles) {
    Ref<InProcessTransport> peer;
    Ref<TransportWrapper> peer_transport;
    {
      absl::MutexLock lock(&mutex_);
      if (!peer_active_) {
        SavedMessage message;
        message.data = std::vector<uint8_t>(data.begin(), data.end());
        message.handles.resize(os_handles.size());
        for (size_t i = 0; i < os_handles.size(); ++i) {
          message.handles[i] = OSHandle::FromIpczOSHandle(os_handles[i]);
        }
        saved_messages_.push_back(std::move(message));
        return IPCZ_RESULT_OK;
      }

      peer = peer_;
      ABSL_ASSERT(peer);
    }

    {
      absl::MutexLock lock(&peer->mutex_);
      peer_transport = peer->transport_;
    }

    if (peer_transport) {
      peer_transport->Notify(data, os_handles);
    }
    return IPCZ_RESULT_OK;
  }

 private:
  void OnPeerActivated() {
    for (;;) {
      std::vector<SavedMessage> saved_messages;
      {
        absl::MutexLock lock(&mutex_);
        ABSL_ASSERT(!peer_active_);
        if (saved_messages_.empty()) {
          peer_active_ = true;
          return;
        }

        std::swap(saved_messages_, saved_messages);
        saved_messages_.clear();
      }

      Ref<TransportWrapper> peer_transport;
      {
        absl::MutexLock lock(&peer_->mutex_);
        peer_transport = peer_->transport_;
      }

      if (!peer_transport) {
        return;
      }

      for (SavedMessage& m : saved_messages) {
        std::vector<IpczOSHandle> os_handles(m.handles.size());
        for (size_t i = 0; i < m.handles.size(); ++i) {
          os_handles[i].size = sizeof(os_handles[i]);
          OSHandle::ToIpczOSHandle(std::move(m.handles[i]), &os_handles[i]);
        }
        peer_transport->Notify(m.data, absl::MakeSpan(os_handles));
      }
    }
  }

  // Effectively const because it's only set once immediately after
  // construction. Hence no need to synchronize access.
  Ref<InProcessTransport> peer_;

  absl::Mutex mutex_;
  Ref<TransportWrapper> transport_ ABSL_GUARDED_BY(mutex_);
  bool peer_active_ ABSL_GUARDED_BY(mutex_) = false;
  std::vector<SavedMessage> saved_messages_ ABSL_GUARDED_BY(mutex_);
};

class InProcessMemory : public RefCounted {
 public:
  explicit InProcessMemory(size_t size)
      : size_(size), data_(new uint8_t[size]) {
    memset(&data_[0], 0, size_);
  }

  size_t size() const { return size_; }
  void* address() const { return &data_[0]; }

 private:
  ~InProcessMemory() override = default;

  const size_t size_;
  const std::unique_ptr<uint8_t[]> data_;
};

IpczResult IPCZ_CDECL CreateTransports(IpczDriverHandle driver_node,
                                       uint32_t flags,
                                       const void* options,
                                       IpczDriverHandle* first_transport,
                                       IpczDriverHandle* second_transport) {
  auto first = MakeRefCounted<InProcessTransport>();
  auto second = MakeRefCounted<InProcessTransport>();
  first->SetPeer(second);
  second->SetPeer(first);
  *first_transport = ToDriverHandle(first.release());
  *second_transport = ToDriverHandle(second.release());
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_CDECL DestroyTransport(IpczDriverHandle driver_transport,
                                       uint32_t flags,
                                       const void* options) {
  Ref<InProcessTransport> transport(
      RefCounted::kAdoptExistingRef,
      ToPtr<InProcessTransport>(driver_transport));
  transport->SetPeer(nullptr);
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_CDECL SerializeTransport(IpczDriverHandle driver_transport,
                                         uint32_t flags,
                                         const void* options,
                                         uint8_t* data,
                                         uint32_t* num_bytes,
                                         struct IpczOSHandle* os_handles,
                                         uint32_t* num_os_handles) {
  const bool need_more_space = *num_bytes < sizeof(driver_transport);
  *num_bytes = sizeof(driver_transport);
  *num_os_handles = 0;
  if (need_more_space) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  memcpy(data, &driver_transport, sizeof(driver_transport));
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_CDECL
DeserializeTransport(IpczDriverHandle driver_node,
                     const uint8_t* data,
                     uint32_t num_bytes,
                     const IpczOSHandle* os_handles,
                     uint32_t num_os_handles,
                     const struct IpczOSProcessHandle* target_process,
                     uint32_t flags,
                     const void* options,
                     IpczDriverHandle* driver_transport) {
  ABSL_ASSERT(num_bytes == sizeof(IpczDriverHandle));
  ABSL_ASSERT(num_os_handles == 0);
  memcpy(driver_transport, data, sizeof(IpczDriverHandle));
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_CDECL ActivateTransport(IpczDriverHandle driver_transport,
                                        IpczHandle transport,
                                        IpczTransportActivityHandler handler,
                                        uint32_t flags,
                                        const void* options) {
  return ToRef<InProcessTransport>(driver_transport)
      .Activate(transport, handler);
}

IpczResult IPCZ_CDECL DeactivateTransport(IpczDriverHandle driver_transport,
                                          uint32_t flags,
                                          const void* options) {
  ToRef<InProcessTransport>(driver_transport).Deactivate();
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_CDECL Transmit(IpczDriverHandle driver_transport,
                               const uint8_t* data,
                               uint32_t num_bytes,
                               const struct IpczOSHandle* os_handles,
                               uint32_t num_os_handles,
                               uint32_t flags,
                               const void* options) {
  return ToRef<InProcessTransport>(driver_transport)
      .Transmit(absl::Span<const uint8_t>(data, num_bytes),
                absl::Span<const IpczOSHandle>(os_handles, num_os_handles));
}

IpczResult IPCZ_CDECL AllocateSharedMemory(uint32_t num_bytes,
                                           uint32_t flags,
                                           const void* options,
                                           IpczDriverHandle* driver_memory) {
  auto memory = MakeRefCounted<InProcessMemory>(num_bytes);
  *driver_memory = ToDriverHandle(memory.release());
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_CDECL
DuplicateSharedMemory(IpczDriverHandle driver_memory,
                      uint32_t flags,
                      const void* options,
                      IpczDriverHandle* new_driver_memory) {
  Ref<InProcessMemory> memory(ToPtr<InProcessMemory>(driver_memory));
  *new_driver_memory = ToDriverHandle(memory.release());
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_CDECL ReleaseSharedMemory(IpczDriverHandle driver_memory,
                                          uint32_t flags,
                                          const void* options) {
  Ref<InProcessMemory> memory(RefCounted::kAdoptExistingRef,
                              ToPtr<InProcessMemory>(driver_memory));
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_CDECL SerializeSharedMemory(IpczDriverHandle driver_memory,
                                            uint32_t flags,
                                            const void* options,
                                            uint8_t* data,
                                            uint32_t* num_bytes,
                                            struct IpczOSHandle* os_handles,
                                            uint32_t* num_os_handles) {
  const bool need_more_space = *num_bytes < sizeof(void*);
  *num_bytes = sizeof(void*);
  *num_os_handles = 0;
  if (need_more_space) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  InProcessMemory* memory = ToPtr<InProcessMemory>(driver_memory);
  *reinterpret_cast<void**>(data) = memory;
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_CDECL DeserializeSharedMemory(const uint8_t* data,
                                              uint32_t num_bytes,
                                              const IpczOSHandle* os_handles,
                                              uint32_t num_os_handles,
                                              uint32_t flags,
                                              const void* options,
                                              uint32_t* region_size,
                                              IpczDriverHandle* driver_memory) {
  if (num_bytes != sizeof(void*) || num_os_handles != 0 || !region_size) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  void* const* ptr_data = reinterpret_cast<void* const*>(data);
  auto& memory = *static_cast<InProcessMemory*>(*ptr_data);
  *region_size = memory.size();
  *driver_memory = ToDriverHandle(&memory);
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_CDECL MapSharedMemory(IpczDriverHandle driver_memory,
                                      uint32_t flags,
                                      const void* options,
                                      void** address,
                                      IpczDriverHandle* driver_mapping) {
  Ref<InProcessMemory> memory(ToPtr<InProcessMemory>(driver_memory));
  *address = memory->address();
  *driver_mapping = ToDriverHandle(memory.release());
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_CDECL UnmapSharedMemory(IpczDriverHandle driver_mapping,
                                        uint32_t flags,
                                        const void* options) {
  Ref<InProcessMemory> memory(RefCounted::kAdoptExistingRef,
                              ToPtr<InProcessMemory>(driver_mapping));
  return IPCZ_RESULT_OK;
}

}  // namespace

const IpczDriver kSingleProcessReferenceDriver = {
    sizeof(kSingleProcessReferenceDriver),
    CreateTransports,
    DestroyTransport,
    SerializeTransport,
    DeserializeTransport,
    ActivateTransport,
    DeactivateTransport,
    Transmit,
    AllocateSharedMemory,
    DuplicateSharedMemory,
    ReleaseSharedMemory,
    SerializeSharedMemory,
    DeserializeSharedMemory,
    MapSharedMemory,
    UnmapSharedMemory,
};

}  // namespace reference_drivers
}  // namespace ipcz
