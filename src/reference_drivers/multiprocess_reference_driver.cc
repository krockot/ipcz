// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "reference_drivers/multiprocess_reference_driver.h"

#include <cstring>
#include <memory>
#include <tuple>
#include <utility>

#include "build/build_config.h"
#include "ipcz/ipcz.h"
#include "reference_drivers/blob.h"
#include "reference_drivers/channel.h"
#include "reference_drivers/memory.h"
#include "reference_drivers/object.h"
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

class MultiprocessTransport : public Object {
 public:
  MultiprocessTransport(Channel channel)
      : Object(kTransport),
        channel_(std::make_unique<Channel>(std::move(channel))) {}
  MultiprocessTransport(const MultiprocessTransport&) = delete;
  MultiprocessTransport& operator=(const MultiprocessTransport&) = delete;

  Channel TakeChannel() {
    if (was_activated_) {
      return {};
    }

    ABSL_ASSERT(channel_);
    Channel channel = std::move(*channel_);
    channel_.reset();
    return channel;
  }

  void Activate(IpczHandle transport,
                IpczTransportActivityHandler activity_handler) {
    was_activated_ = true;
    transport_ = transport;
    activity_handler_ = activity_handler;
    channel_->Listen(
        [transport = WrapRefCounted(this)](Channel::Message message) {
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
    std::vector<OSHandle> handles(os_handles.size());
    for (size_t i = 0; i < os_handles.size(); ++i) {
      handles[i] = OSHandle::FromIpczOSHandle(os_handles[i]);
    }
    channel_->Send(
        Channel::Message(Channel::Data(data), absl::MakeSpan(handles)));
    return IPCZ_RESULT_OK;
  }

 private:
  ~MultiprocessTransport() override = default;

  bool OnMessage(const Channel::Message& message) {
    std::vector<IpczOSHandle> os_handles(message.handles.size());
    for (size_t i = 0; i < os_handles.size(); ++i) {
      os_handles[i].size = sizeof(os_handles[i]);
      OSHandle::ToIpczOSHandle(std::move(message.handles[i]), &os_handles[i]);
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
  std::unique_ptr<Channel> channel_;
};

class MultiprocessMemoryMapping : public Object {
 public:
  MultiprocessMemoryMapping(Memory::Mapping mapping)
      : Object(kMapping), mapping_(std::move(mapping)) {}

  void* address() const { return mapping_.base(); }

 private:
  ~MultiprocessMemoryMapping() override = default;

  const Memory::Mapping mapping_;
};

class MultiprocessMemory : public Object {
 public:
  explicit MultiprocessMemory(size_t num_bytes)
      : Object(kMemory), memory_(num_bytes) {}
  MultiprocessMemory(OSHandle handle, size_t num_bytes)
      : Object(kMemory), memory_(std::move(handle), num_bytes) {}

  size_t size() const { return memory_.size(); }

  Ref<MultiprocessMemory> Clone() {
    return MakeRefCounted<MultiprocessMemory>(memory_.Clone().TakeHandle(),
                                              memory_.size());
  }

  Ref<MultiprocessMemoryMapping> Map() {
    return MakeRefCounted<MultiprocessMemoryMapping>(memory_.Map());
  }

  OSHandle TakeHandle() { return memory_.TakeHandle(); }

 private:
  ~MultiprocessMemory() override = default;

  Memory memory_;
};

struct IPCZ_ALIGN(8) SerializedObject {
  Object::Type type;

  // Size of the memory region iff `type` is kMemory; otherwise zero.
  uint32_t memory_size;
};

IpczResult IPCZ_CDECL Close(IpczDriverHandle handle,
                            uint32_t flags,
                            const void* options) {
  Ref<Object> object = Object::ReleaseFromHandle(handle);
  if (!object) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  object->Close();
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_CDECL Serialize(IpczDriverHandle handle,
                                uint32_t flags,
                                const void* options,
                                uint8_t* data,
                                uint32_t* num_bytes,
                                struct IpczOSHandle* os_handles,
                                uint32_t* num_os_handles) {
  Object* object = Object::FromHandle(handle);
  if (!object) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  size_t required_num_bytes = sizeof(SerializedObject);
  size_t required_num_os_handles = 0;
  switch (object->type()) {
    case Object::kTransport:
      required_num_os_handles = 1;
      break;

    case Object::kMemory:
      required_num_os_handles = 1;
      break;

    case Object::kBlob:
      required_num_bytes += static_cast<Blob*>(object)->message().size();
      required_num_os_handles = static_cast<Blob*>(object)->handles().size();
      break;

    default:
      return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  const bool need_more_space = *num_bytes < required_num_bytes ||
                               *num_os_handles < required_num_os_handles;
  *num_bytes = required_num_bytes;
  *num_os_handles = required_num_os_handles;
  if (need_more_space) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  auto& header = *reinterpret_cast<SerializedObject*>(data);
  header.type = object->type();
  header.memory_size = 0;

  if (object->type() == Object::kTransport) {
    auto transport = object->ReleaseAs<MultiprocessTransport>();
    Channel channel = transport->TakeChannel();
    if (!channel.is_valid()) {
      std::ignore = transport.release();
      return IPCZ_RESULT_FAILED_PRECONDITION;
    }

    os_handles[0].size = sizeof(os_handles[0]);
    bool ok = OSHandle::ToIpczOSHandle(channel.TakeHandle(), &os_handles[0]);
    ABSL_ASSERT(ok);
    return IPCZ_RESULT_OK;
  }

  if (object->type() == Object::kMemory) {
    auto memory = object->ReleaseAs<MultiprocessMemory>();
    header.memory_size = static_cast<uint32_t>(memory->size());
    bool ok = OSHandle::ToIpczOSHandle(memory->TakeHandle(), &os_handles[0]);
    ABSL_ASSERT(ok);
    return IPCZ_RESULT_OK;
  }

  if (object->type() == Object::kBlob) {
    auto blob = object->ReleaseAs<Blob>();
    uint8_t* blob_data = reinterpret_cast<uint8_t*>(&header + 1);
    memcpy(blob_data, blob->message().data(), blob->message().size());
    for (size_t i = 0; i < blob->handles().size(); ++i) {
      bool ok = OSHandle::ToIpczOSHandle(std::move(blob->handles()[i]),
                                         &os_handles[i]);
      ABSL_ASSERT(ok);
    }
    return IPCZ_RESULT_OK;
  }

  return IPCZ_RESULT_INVALID_ARGUMENT;
}

IpczResult IPCZ_CDECL Deserialize(IpczDriverHandle driver_node,
                                  const uint8_t* data,
                                  uint32_t num_bytes,
                                  const IpczOSHandle* os_handles,
                                  uint32_t num_os_handles,
                                  uint32_t flags,
                                  const void* options,
                                  IpczDriverHandle* driver_handle) {
  if (num_bytes < sizeof(SerializedObject)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  const SerializedObject& header =
      *reinterpret_cast<const SerializedObject*>(data);
  if (header.type == Object::kBlob) {
    std::vector<OSHandle> handles(num_os_handles);
    for (size_t i = 0; i < num_os_handles; ++i) {
      handles[i] = OSHandle::FromIpczOSHandle(os_handles[i]);
      if (!handles[i].is_valid()) {
        return IPCZ_RESULT_INVALID_ARGUMENT;
      }
    }

    const char* string_data = reinterpret_cast<const char*>(&header + 1);
    Ref<Blob> blob = MakeRefCounted<Blob>(
        std::string_view(string_data, num_bytes), absl::MakeSpan(handles));
    *driver_handle = ToDriverHandle(blob.release());
    return IPCZ_RESULT_OK;
  }

  if (num_bytes != sizeof(SerializedObject) || num_os_handles != 1) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  OSHandle handle = OSHandle::FromIpczOSHandle(os_handles[0]);
  if (!handle.is_valid()) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (header.type == Object::kTransport) {
    Channel channel(std::move(handle));
    if (!channel.is_valid()) {
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    auto transport = MakeRefCounted<MultiprocessTransport>(std::move(channel));
    *driver_handle = ToDriverHandle(transport.release());
    return IPCZ_RESULT_OK;
  }

  if (header.type == Object::kMemory) {
    auto memory = MakeRefCounted<MultiprocessMemory>(std::move(handle),
                                                     header.memory_size);
    *driver_handle = ToDriverHandle(memory.release());
    return IPCZ_RESULT_OK;
  }

  return IPCZ_RESULT_INVALID_ARGUMENT;
}

IpczResult IPCZ_CDECL CreateTransports(IpczDriverHandle driver_node,
                                       uint32_t flags,
                                       const void* options,
                                       IpczDriverHandle* first_transport,
                                       IpczDriverHandle* second_transport) {
  auto [first_channel, second_channel] = Channel::CreateChannelPair();
  auto first = MakeRefCounted<MultiprocessTransport>(std::move(first_channel));
  auto second =
      MakeRefCounted<MultiprocessTransport>(std::move(second_channel));
  *first_transport = ToDriverHandle(first.release());
  *second_transport = ToDriverHandle(second.release());
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_CDECL
ActivateTransport(IpczDriverHandle driver_transport,
                  IpczHandle transport,
                  IpczTransportActivityHandler activity_handler,
                  uint32_t flags,
                  const void* options) {
  ToRef<MultiprocessTransport>(driver_transport)
      .Activate(transport, activity_handler);
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_CDECL DeactivateTransport(IpczDriverHandle driver_transport,
                                          uint32_t flags,
                                          const void* options) {
  ToRef<MultiprocessTransport>(driver_transport).Deactivate();
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_CDECL Transmit(IpczDriverHandle driver_transport,
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

IpczResult IPCZ_CDECL AllocateSharedMemory(uint32_t num_bytes,
                                           uint32_t flags,
                                           const void* options,
                                           IpczDriverHandle* driver_memory) {
  auto memory = MakeRefCounted<MultiprocessMemory>(num_bytes);
  *driver_memory = ToDriverHandle(memory.release());
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_CDECL
DuplicateSharedMemory(IpczDriverHandle driver_memory,
                      uint32_t flags,
                      const void* options,
                      IpczDriverHandle* new_driver_memory) {
  auto memory = ToRef<MultiprocessMemory>(driver_memory).Clone();
  *new_driver_memory = ToDriverHandle(memory.release());
  return IPCZ_RESULT_OK;
}

IpczResult GetSharedMemoryInfo(IpczDriverHandle driver_memory,
                               uint32_t flags,
                               const void* options,
                               uint32_t* size) {
  Object* object = Object::FromHandle(driver_memory);
  if (!object || object->type() != Object::kMemory) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  *size =
      static_cast<uint32_t>(static_cast<MultiprocessMemory*>(object)->size());
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_CDECL MapSharedMemory(IpczDriverHandle driver_memory,
                                      uint32_t flags,
                                      const void* options,
                                      void** address,
                                      IpczDriverHandle* driver_mapping) {
  auto mapping = ToRef<MultiprocessMemory>(driver_memory).Map();
  *address = mapping->address();
  *driver_mapping = ToDriverHandle(mapping.release());
  return IPCZ_RESULT_OK;
}

}  // namespace

const IpczDriver kMultiprocessReferenceDriver = {
    sizeof(kMultiprocessReferenceDriver),
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
};

IpczDriverHandle CreateTransportFromChannel(Channel channel) {
  auto transport = MakeRefCounted<MultiprocessTransport>(std::move(channel));
  return ToDriverHandle(transport.release());
}

}  // namespace reference_drivers
}  // namespace ipcz
