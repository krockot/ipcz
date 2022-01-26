// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "drivers/multiprocess_reference_driver.h"

#include <memory>
#include <tuple>
#include <utility>

#include "build/build_config.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/channel.h"
#include "os/handle.h"
#include "os/memory.h"
#include "util/handle_util.h"

#if defined(OS_WIN)
#define CDECL __cdecl
#else
#define CDECL
#endif

namespace ipcz {
namespace drivers {

namespace {

class MultiprocessTransport : public mem::RefCounted {
 public:
  MultiprocessTransport(os::Channel channel)
      : channel_(std::make_unique<os::Channel>(std::move(channel))) {}
  MultiprocessTransport(const MultiprocessTransport&) = delete;
  MultiprocessTransport& operator=(const MultiprocessTransport&) = delete;

  os::Channel TakeChannel() {
    if (was_activated_) {
      return {};
    }

    ABSL_ASSERT(channel_);
    os::Channel channel = std::move(*channel_);
    channel_.reset();
    return channel;
  }

  void Activate(IpczHandle transport,
                IpczTransportActivityHandler activity_handler) {
    was_activated_ = true;
    transport_ = transport;
    activity_handler_ = activity_handler;
    channel_->Listen(
        [transport = mem::WrapRefCounted(this)](os::Channel::Message message) {
          if (!transport->OnMessage(message)) {
            transport->OnError();
            return false;
          }
          return true;
        });
  }

  void Deactivate() {
    channel_->StopListening();
    if (activity_handler_) {
      activity_handler_(transport_, nullptr, 0, nullptr, 0,
                        IPCZ_TRANSPORT_ACTIVITY_DEACTIVATED, nullptr);
    }
  }

  IpczResult Transmit(absl::Span<const uint8_t> data,
                      absl::Span<const IpczOSHandle> os_handles) {
    std::vector<os::Handle> handles(os_handles.size());
    for (size_t i = 0; i < os_handles.size(); ++i) {
      handles[i] = os::Handle::FromIpczOSHandle(os_handles[i]);
    }
    channel_->Send(
        os::Channel::Message(os::Channel::Data(data), absl::MakeSpan(handles)));
    return IPCZ_RESULT_OK;
  }

 private:
  ~MultiprocessTransport() override = default;

  bool OnMessage(const os::Channel::Message& message) {
    std::vector<IpczOSHandle> os_handles(message.handles.size());
    for (size_t i = 0; i < os_handles.size(); ++i) {
      os_handles[i].size = sizeof(os_handles[i]);
      os::Handle::ToIpczOSHandle(std::move(message.handles[i]), &os_handles[i]);
    }

    ABSL_ASSERT(activity_handler_);
    IpczResult result = activity_handler_(
        transport_, message.data.data(),
        static_cast<uint32_t>(message.data.size()), os_handles.data(),
        static_cast<uint32_t>(os_handles.size()), IPCZ_NO_FLAGS, nullptr);
    return result == IPCZ_RESULT_OK || result == IPCZ_RESULT_UNIMPLEMENTED;
  }

  void OnError() {
    activity_handler_(transport_, nullptr, 0, nullptr, 0,
                      IPCZ_TRANSPORT_ACTIVITY_ERROR, nullptr);
  }

  // Access to these fields is not synchronized: ipcz is responsible for
  // ensuring that a transport is deactivated before this handle is invalidated,
  // and also for ensuring that Transmit() is never called on the transport once
  // deactivated. The driver is responsible for ensuring that it doesn't use
  // `transport_` after deactivation either.
  //
  // Because activation and de-activation are also one-time operations, a well
  // behaved application and ipcz implementation cannot elicit race conditions.

  IpczHandle transport_ = IPCZ_INVALID_HANDLE;
  IpczTransportActivityHandler activity_handler_;
  bool was_activated_ = false;
  std::unique_ptr<os::Channel> channel_;
};

class MultiprocessMemoryMapping {
 public:
  MultiprocessMemoryMapping(os::Memory::Mapping mapping)
      : mapping_(std::move(mapping)) {}
  ~MultiprocessMemoryMapping() = default;

  void* address() const { return mapping_.base(); }

 private:
  const os::Memory::Mapping mapping_;
};

class MultiprocessMemory {
 public:
  explicit MultiprocessMemory(size_t num_bytes) : memory_(num_bytes) {}
  MultiprocessMemory(os::Handle handle, size_t num_bytes)
      : memory_(std::move(handle), num_bytes) {}
  ~MultiprocessMemory() = default;

  size_t size() const { return memory_.size(); }

  std::unique_ptr<MultiprocessMemory> Clone() {
    return std::make_unique<MultiprocessMemory>(memory_.Clone().TakeHandle(),
                                                memory_.size());
  }

  std::unique_ptr<MultiprocessMemoryMapping> Map() {
    return std::make_unique<MultiprocessMemoryMapping>(memory_.Map());
  }

  os::Handle TakeHandle() { return memory_.TakeHandle(); }

 private:
  os::Memory memory_;
};

IpczResult CDECL CreateTransports(IpczDriverHandle driver_node,
                                  uint32_t flags,
                                  const void* options,
                                  IpczDriverHandle* first_transport,
                                  IpczDriverHandle* second_transport) {
  os::Channel first_channel, second_channel;
  std::tie(first_channel, second_channel) = os::Channel::CreateChannelPair();
  auto first =
      mem::MakeRefCounted<MultiprocessTransport>(std::move(first_channel));
  auto second =
      mem::MakeRefCounted<MultiprocessTransport>(std::move(second_channel));
  *first_transport = ToDriverHandle(first.release());
  *second_transport = ToDriverHandle(second.release());
  return IPCZ_RESULT_OK;
}

IpczResult CDECL DestroyTransport(IpczDriverHandle driver_transport,
                                  uint32_t flags,
                                  const void* options) {
  mem::Ref<MultiprocessTransport> transport(
      mem::RefCounted::kAdoptExistingRef,
      ToPtr<MultiprocessTransport>(driver_transport));
  return IPCZ_RESULT_OK;
}

IpczResult CDECL SerializeTransport(IpczDriverHandle driver_transport,
                                    uint32_t flags,
                                    const void* options,
                                    uint8_t* data,
                                    uint32_t* num_bytes,
                                    struct IpczOSHandle* os_handles,
                                    uint32_t* num_os_handles) {
  const bool need_more_space = *num_os_handles < 1;
  *num_bytes = 0;
  *num_os_handles = 1;
  if (need_more_space) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  mem::Ref<MultiprocessTransport> transport(
      mem::RefCounted::kAdoptExistingRef,
      ToPtr<MultiprocessTransport>(driver_transport));
  os::Channel channel = transport->TakeChannel();
  if (!channel.is_valid()) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  os_handles[0].size = sizeof(os_handles[0]);
  if (!os::Handle::ToIpczOSHandle(channel.TakeHandle(), &os_handles[0])) {
    return IPCZ_RESULT_UNKNOWN;
  }

  return IPCZ_RESULT_OK;
}

IpczResult CDECL
DeserializeTransport(IpczDriverHandle driver_node,
                     const uint8_t* data,
                     uint32_t num_bytes,
                     const IpczOSHandle* os_handles,
                     uint32_t num_os_handles,
                     const struct IpczOSProcessHandle* target_process,
                     uint32_t flags,
                     const void* options,
                     IpczDriverHandle* driver_transport) {
  if (num_bytes != 0 || num_os_handles != 1) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  os::Channel channel(os::Handle::FromIpczOSHandle(os_handles[0]));
  if (!channel.is_valid()) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  auto transport =
      mem::MakeRefCounted<MultiprocessTransport>(std::move(channel));
  *driver_transport = ToDriverHandle(transport.release());
  return IPCZ_RESULT_OK;
}

IpczResult CDECL
ActivateTransport(IpczDriverHandle driver_transport,
                  IpczHandle transport,
                  IpczTransportActivityHandler activity_handler,
                  uint32_t flags,
                  const void* options) {
  ToRef<MultiprocessTransport>(driver_transport)
      .Activate(transport, activity_handler);
  return IPCZ_RESULT_OK;
}

IpczResult CDECL DeactivateTransport(IpczDriverHandle driver_transport,
                                     uint32_t flags,
                                     const void* options) {
  ToRef<MultiprocessTransport>(driver_transport).Deactivate();
  return IPCZ_RESULT_OK;
}

IpczResult CDECL Transmit(IpczDriverHandle driver_transport,
                          const uint8_t* data,
                          uint32_t num_bytes,
                          const struct IpczOSHandle* os_handles,
                          uint32_t num_os_handles,
                          uint32_t flags,
                          const void* options) {
  return ToRef<MultiprocessTransport>(driver_transport)
      .Transmit(absl::Span<const uint8_t>(data, num_bytes),
                absl::Span<const IpczOSHandle>(os_handles, num_os_handles));
}

IpczResult CDECL AllocateSharedMemory(uint32_t num_bytes,
                                      uint32_t flags,
                                      const void* options,
                                      IpczDriverHandle* driver_memory) {
  auto memory = std::make_unique<MultiprocessMemory>(num_bytes);
  *driver_memory = ToDriverHandle(memory.release());
  return IPCZ_RESULT_OK;
}

IpczResult CDECL DuplicateSharedMemory(IpczDriverHandle driver_memory,
                                       uint32_t flags,
                                       const void* options,
                                       IpczDriverHandle* new_driver_memory) {
  auto memory = ToRef<MultiprocessMemory>(driver_memory).Clone();
  *new_driver_memory = ToDriverHandle(memory.release());
  return IPCZ_RESULT_OK;
}

IpczResult CDECL ReleaseSharedMemory(IpczDriverHandle driver_memory,
                                     uint32_t flags,
                                     const void* options) {
  std::unique_ptr<MultiprocessMemory> memory(
      ToPtr<MultiprocessMemory>(driver_memory));
  return IPCZ_RESULT_OK;
}

IpczResult CDECL SerializeSharedMemory(IpczDriverHandle driver_memory,
                                       uint32_t flags,
                                       const void* options,
                                       uint8_t* data,
                                       uint32_t* num_bytes,
                                       struct IpczOSHandle* os_handles,
                                       uint32_t* num_os_handles) {
  const bool need_more_space = *num_os_handles < 1 || *num_bytes < 4;
  *num_bytes = 4;
  *num_os_handles = 1;
  if (need_more_space) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  std::unique_ptr<MultiprocessMemory> memory(
      ToPtr<MultiprocessMemory>(driver_memory));
  reinterpret_cast<uint32_t*>(data)[0] = static_cast<uint32_t>(memory->size());
  if (!os::Handle::ToIpczOSHandle(memory->TakeHandle(), &os_handles[0])) {
    std::ignore = memory.release();
    return IPCZ_RESULT_UNKNOWN;
  }

  return IPCZ_RESULT_OK;
}

IpczResult CDECL DeserializeSharedMemory(const uint8_t* data,
                                         uint32_t num_bytes,
                                         const IpczOSHandle* os_handles,
                                         uint32_t num_os_handles,
                                         uint32_t flags,
                                         const void* options,
                                         uint32_t* region_size,
                                         IpczDriverHandle* driver_memory) {
  if (num_bytes != 4 || num_os_handles != 1 || !region_size) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  os::Handle handle = os::Handle::FromIpczOSHandle(os_handles[0]);
  if (!handle.is_valid()) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  const uint32_t size = reinterpret_cast<const uint32_t*>(data)[0];
  auto memory = std::make_unique<MultiprocessMemory>(std::move(handle), size);

  *region_size = size;
  *driver_memory = ToDriverHandle(memory.release());
  return IPCZ_RESULT_OK;
}

IpczResult CDECL MapSharedMemory(IpczDriverHandle driver_memory,
                                 uint32_t flags,
                                 const void* options,
                                 void** address,
                                 IpczDriverHandle* driver_mapping) {
  auto mapping = ToRef<MultiprocessMemory>(driver_memory).Map();
  *address = mapping->address();
  *driver_mapping = ToDriverHandle(mapping.release());
  return IPCZ_RESULT_OK;
}

IpczResult CDECL UnmapSharedMemory(IpczDriverHandle driver_mapping,
                                   uint32_t flags,
                                   const void* options) {
  std::unique_ptr<MultiprocessMemoryMapping> mapping(
      ToPtr<MultiprocessMemoryMapping>(driver_mapping));
  return IPCZ_RESULT_OK;
}

}  // namespace

const IpczDriver kMultiprocessReferenceDriver = {
    sizeof(kMultiprocessReferenceDriver),
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

}  // namespace drivers
}  // namespace ipcz
