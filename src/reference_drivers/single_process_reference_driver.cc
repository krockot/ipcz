// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "reference_drivers/single_process_reference_driver.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "ipcz/ipcz.h"
#include "reference_drivers/handle_util.h"
#include "reference_drivers/object.h"
#include "reference_drivers/os_handle.h"
#include "reference_drivers/random.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/ref_counted.h"

namespace ipcz::reference_drivers {

namespace {

class TransportWrapper : public RefCounted {
 public:
  TransportWrapper(IpczHandle transport,
                   IpczTransportActivityHandler activity_handler)
      : transport_(transport), activity_handler_(activity_handler) {}

  IpczResult Notify(absl::Span<const uint8_t> data,
                    absl::Span<const IpczDriverHandle> handles) {
    IpczResult result = activity_handler_(
        transport_, data.data(), static_cast<uint32_t>(data.size()),
        handles.data(), static_cast<uint32_t>(handles.size()), IPCZ_NO_FLAGS,
        nullptr);
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
  std::vector<IpczDriverHandle> handles;
};

class InProcessTransport : public Object {
 public:
  InProcessTransport() : Object(kTransport) {}
  ~InProcessTransport() override = default;

  // Object:
  IpczResult Close() override {
    SetPeer(nullptr);
    return IPCZ_RESULT_OK;
  }

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
                      absl::Span<const IpczDriverHandle> handles) {
    Ref<InProcessTransport> peer;
    Ref<TransportWrapper> peer_transport;
    {
      absl::MutexLock lock(&mutex_);
      if (!peer_active_) {
        SavedMessage message;
        message.data = std::vector<uint8_t>(data.begin(), data.end());
        message.handles =
            std::vector<IpczDriverHandle>(handles.begin(), handles.end());
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
      peer_transport->Notify(data, handles);
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
        peer_transport->Notify(m.data, m.handles);
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

class InProcessMemory : public Object {
 public:
  explicit InProcessMemory(size_t size)
      : Object(kMemory), size_(size), data_(new uint8_t[size]) {
    memset(&data_[0], 0, size_);
  }

  size_t size() const { return size_; }
  void* address() const { return &data_[0]; }

 private:
  ~InProcessMemory() override = default;

  const size_t size_;
  const std::unique_ptr<uint8_t[]> data_;
};

class InProcessMapping : public Object {
 public:
  explicit InProcessMapping(Ref<InProcessMemory> memory)
      : Object(kMapping), memory_(std::move(memory)) {}

  size_t size() const { return memory_->size(); }
  void* address() const { return memory_->address(); }

 private:
  ~InProcessMapping() override = default;

  const Ref<InProcessMemory> memory_;
};

IpczResult IPCZ_API Close(IpczDriverHandle handle,
                          uint32_t flags,
                          const void* options) {
  Ref<Object> object = Object::ReleaseFromHandle(handle);
  if (!object) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  return object->Close();
}

IpczResult IPCZ_API Serialize(IpczDriverHandle handle,
                              IpczDriverHandle transport,
                              uint32_t flags,
                              const void* options,
                              uint8_t* data,
                              uint32_t* num_bytes,
                              IpczDriverHandle* handles,
                              uint32_t* num_handles) {
  Object* object = Object::FromHandle(handle);
  if (!object) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (object->type() == Object::kUnserializableGarbage) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  // Since this is all in-process, "serialization" can just copy the handle.
  const uint32_t data_capacity = num_bytes ? *num_bytes : 0;
  constexpr size_t kRequiredNumBytes = sizeof(IpczDriverHandle);
  if (num_bytes) {
    *num_bytes = kRequiredNumBytes;
  }
  if (num_handles) {
    *num_handles = 0;
  }
  if (data_capacity < kRequiredNumBytes) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  *reinterpret_cast<IpczDriverHandle*>(data) = handle;
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_API Deserialize(const uint8_t* data,
                                uint32_t num_bytes,
                                const IpczDriverHandle* handles,
                                uint32_t num_handles,
                                IpczDriverHandle transport,
                                uint32_t flags,
                                const void* options,
                                IpczDriverHandle* driver_handle) {
  ABSL_ASSERT(num_bytes == sizeof(IpczDriverHandle));
  ABSL_ASSERT(num_handles == 0);
  *driver_handle = *reinterpret_cast<const IpczDriverHandle*>(data);
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_API CreateTransports(IpczDriverHandle transport0,
                                     IpczDriverHandle transport1,
                                     uint32_t flags,
                                     const void* options,
                                     IpczDriverHandle* new_transport0,
                                     IpczDriverHandle* new_transport1) {
  auto first = MakeRefCounted<InProcessTransport>();
  auto second = MakeRefCounted<InProcessTransport>();
  first->SetPeer(second);
  second->SetPeer(first);
  *new_transport0 = ToDriverHandle(first.release());
  *new_transport1 = ToDriverHandle(second.release());
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_API ActivateTransport(IpczDriverHandle driver_transport,
                                      IpczHandle transport,
                                      IpczTransportActivityHandler handler,
                                      uint32_t flags,
                                      const void* options) {
  return ToRef<InProcessTransport>(driver_transport)
      .Activate(transport, handler);
}

IpczResult IPCZ_API DeactivateTransport(IpczDriverHandle driver_transport,
                                        uint32_t flags,
                                        const void* options) {
  ToRef<InProcessTransport>(driver_transport).Deactivate();
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_API Transmit(IpczDriverHandle driver_transport,
                             const uint8_t* data,
                             uint32_t num_bytes,
                             const IpczDriverHandle* handles,
                             uint32_t num_handles,
                             uint32_t flags,
                             const void* options) {
  return ToRef<InProcessTransport>(driver_transport)
      .Transmit(absl::MakeSpan(data, num_bytes),
                absl::MakeSpan(handles, num_handles));
}

IpczResult IPCZ_API AllocateSharedMemory(uint64_t num_bytes,
                                         uint32_t flags,
                                         const void* options,
                                         IpczDriverHandle* driver_memory) {
  if (num_bytes > std::numeric_limits<size_t>::max()) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  auto memory = MakeRefCounted<InProcessMemory>(static_cast<size_t>(num_bytes));
  *driver_memory = ToDriverHandle(memory.release());
  return IPCZ_RESULT_OK;
}

IpczResult GetSharedMemoryInfo(IpczDriverHandle driver_memory,
                               uint32_t flags,
                               const void* options,
                               IpczSharedMemoryInfo* info) {
  Object* object = Object::FromHandle(driver_memory);
  if (!object || object->type() != Object::kMemory || !info ||
      info->size < sizeof(IpczSharedMemoryInfo)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  info->region_num_bytes = static_cast<InProcessMemory*>(object)->size();
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_API DuplicateSharedMemory(IpczDriverHandle driver_memory,
                                          uint32_t flags,
                                          const void* options,
                                          IpczDriverHandle* new_driver_memory) {
  Ref<InProcessMemory> memory(ToPtr<InProcessMemory>(driver_memory));
  *new_driver_memory = ToDriverHandle(memory.release());
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_API MapSharedMemory(IpczDriverHandle driver_memory,
                                    uint32_t flags,
                                    const void* options,
                                    void** address,
                                    IpczDriverHandle* driver_mapping) {
  Ref<InProcessMemory> memory(ToPtr<InProcessMemory>(driver_memory));
  auto mapping = MakeRefCounted<InProcessMapping>(std::move(memory));
  *address = mapping->address();
  *driver_mapping = ToDriverHandle(mapping.release());
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_API GenerateRandomBytes(uint32_t num_bytes,
                                        uint32_t flags,
                                        const void* options,
                                        void* buffer) {
  RandomBytes(absl::MakeSpan(static_cast<uint8_t*>(buffer), num_bytes));
  return IPCZ_RESULT_OK;
}

}  // namespace

const IpczDriver kSingleProcessReferenceDriver = {
    sizeof(kSingleProcessReferenceDriver),
    Close,
    Serialize,
    Deserialize,
    CreateTransports,
    ActivateTransport,
    DeactivateTransport,
    Transmit,
    AllocateSharedMemory,
    GetSharedMemoryInfo,
    DuplicateSharedMemory,
    MapSharedMemory,
    GenerateRandomBytes,
};

IpczDriverHandle CreateUnserializableTestObject() {
  Ref<Object> garbage = MakeRefCounted<Object>(Object::kUnserializableGarbage);
  return ToDriverHandle(garbage.release());
}

}  // namespace ipcz::reference_drivers
