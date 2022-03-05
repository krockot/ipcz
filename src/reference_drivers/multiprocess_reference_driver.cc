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
#include "reference_drivers/os_handle.h"
#include "reference_drivers/wrapped_os_handle.h"
#include "util/handle_util.h"
#include "util/ref_counted.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

namespace ipcz::reference_drivers {

namespace {

#if BUILDFLAG(IS_WIN)
HANDLE TransferHandle(HANDLE handle,
                      const OSProcess& from_process,
                      const OSProcess& to_process) {
  if (!from_process.is_valid() || !to_process.is_valid()) {
    // Assume that if we don't known one of the process's, the handle must
    // already be ours.
    return handle;
  }

  HANDLE new_handle;
  BOOL result = ::DuplicateHandle(
      from_process.handle(), handle, to_process.handle(), &new_handle, 0, FALSE,
      DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE);
  ABSL_ASSERT(result);
  return new_handle;
}
#endif

class MultiprocessTransport : public Object {
 public:
  MultiprocessTransport(Channel channel,
                        OSProcess remote_process,
                        MultiprocessTransportSource source,
                        MultiprocessTransportTarget target)
      : Object(kTransport),
        remote_process_(std::move(remote_process)),
        source_(source),
        target_(target),
        channel_(std::make_unique<Channel>(std::move(channel))) {}
  MultiprocessTransport(const MultiprocessTransport&) = delete;
  MultiprocessTransport& operator=(const MultiprocessTransport&) = delete;

  MultiprocessTransportSource source() const { return source_; }
  MultiprocessTransportTarget target() const { return target_; }
  const OSProcess& remote_process() const { return remote_process_; }

  bool is_serializable() const {
    return !was_activated_ && channel_ && channel_->is_valid();
  }

  static MultiprocessTransport& FromHandle(IpczDriverHandle handle) {
    return Object::FromHandle(handle)->As<MultiprocessTransport>();
  }

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
        },
        [transport = WrapRefCounted(this)]() { transport->OnError(); });
  }

  void Deactivate() {
    channel_->StopListening();
    if (activity_handler_) {
      activity_handler_(transport_, nullptr, 0, nullptr, 0,
                        IPCZ_TRANSPORT_ACTIVITY_DEACTIVATED, nullptr);
    }
  }

  IpczResult Transmit(absl::Span<const uint8_t> data,
                      absl::Span<const IpczDriverHandle> handles) {
    std::vector<OSHandle> os_handles(handles.size());
    for (size_t i = 0; i < handles.size(); ++i) {
      ABSL_ASSERT(Object::FromHandle(handles[i])->type() == Object::kOSHandle);
      os_handles[i] = Object::ReleaseFromHandleAs<WrappedOSHandle>(handles[i])
                          ->TakeHandle();
    }
    channel_->Send(
        Channel::Message(Channel::Data(data), absl::MakeSpan(os_handles)));
    return IPCZ_RESULT_OK;
  }

 private:
  ~MultiprocessTransport() override = default;

  bool OnMessage(const Channel::Message& message) {
    std::vector<IpczDriverHandle> handles(message.handles.size());
    for (size_t i = 0; i < handles.size(); ++i) {
      handles[i] = Object::ReleaseAsHandle(
          MakeRefCounted<WrappedOSHandle>(std::move(message.handles[i])));
    }

    ABSL_ASSERT(activity_handler_);
    IpczResult result = activity_handler_(
        transport_, message.data.data(),
        static_cast<uint32_t>(message.data.size()), handles.data(),
        static_cast<uint32_t>(handles.size()), IPCZ_NO_FLAGS, nullptr);
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

  const OSProcess remote_process_;
  const MultiprocessTransportSource source_;
  const MultiprocessTransportTarget target_;
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

  union {
    uint32_t extra_data;

    // Set iff `type` is kTransport.
    struct {
      MultiprocessTransportSource source;
      MultiprocessTransportTarget target;
    };

    // Size of the memory region iff `type` is kMemory; size of the message data
    // iff `type` is kBlob.
    uint32_t size;
  };
};

IpczResult IPCZ_API Close(IpczDriverHandle handle,
                          uint32_t flags,
                          const void* options) {
  Ref<Object> object = Object::ReleaseFromHandle(handle);
  if (!object) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  object->Close();
  return IPCZ_RESULT_OK;
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

  size_t required_num_bytes = sizeof(SerializedObject);
  size_t required_num_handles = 0;
  switch (object->type()) {
    case Object::kTransport: {
      if (!object->As<MultiprocessTransport>().is_serializable()) {
        return IPCZ_RESULT_INVALID_ARGUMENT;
      }
      required_num_handles = 1;
      break;
    }

    case Object::kMemory:
      required_num_handles = 1;
      break;

    case Object::kBlob:
      required_num_bytes += static_cast<Blob*>(object)->message().size();
      required_num_handles = static_cast<Blob*>(object)->handles().size();
      break;

    default:
      return IPCZ_RESULT_INVALID_ARGUMENT;
  }

#if BUILDFLAG(IS_WIN)
  // On Windows, handles are serialized as additional inline data.
  required_num_bytes += sizeof(HANDLE) * required_num_handles;
  required_num_handles = 0;
#endif

  const uint32_t data_capacity = num_bytes ? *num_bytes : 0;
  const uint32_t handle_capacity = num_handles ? *num_handles : 0;
  if (num_bytes) {
    *num_bytes = required_num_bytes;
  }
  if (num_handles) {
    *num_handles = required_num_handles;
  }
  const bool need_more_space = data_capacity < required_num_bytes ||
                               handle_capacity < required_num_handles;
  if (need_more_space) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  auto& header = *reinterpret_cast<SerializedObject*>(data);
  header.type = object->type();
  header.extra_data = 0;

#if BUILDFLAG(IS_WIN)
  if (transport == IPCZ_INVALID_DRIVER_HANDLE) {
    return IPCZ_RESULT_ABORTED;
  }

  HANDLE* handle_data = reinterpret_cast<HANDLE*>(&header + 1);
  const OSProcess current_process = OSProcess::GetCurrent();
  const OSProcess& remote_process =
      MultiprocessTransport::FromHandle(transport).remote_process();
#endif

  if (object->type() == Object::kTransport) {
    auto released_transport = object->ReleaseAs<MultiprocessTransport>();
    header.source = released_transport->source();
    header.target = released_transport->target();
    Channel channel = released_transport->TakeChannel();
    ABSL_ASSERT(channel.is_valid());
#if BUILDFLAG(IS_WIN)
    handle_data[0] = TransferHandle(channel.TakeHandle().ReleaseHandle(),
                                    current_process, remote_process);
#else
    handles[0] = Object::ReleaseAsHandle(
        MakeRefCounted<WrappedOSHandle>(channel.TakeHandle()));
#endif
    return IPCZ_RESULT_OK;
  }

  if (object->type() == Object::kMemory) {
    auto memory = object->ReleaseAs<MultiprocessMemory>();
    header.size = static_cast<uint32_t>(memory->size());
#if BUILDFLAG(IS_WIN)
    handle_data[0] = TransferHandle(memory->TakeHandle().ReleaseHandle(),
                                    current_process, remote_process);
#else
    handles[0] = Object::ReleaseAsHandle(
        MakeRefCounted<WrappedOSHandle>(memory->TakeHandle()));
#endif
    return IPCZ_RESULT_OK;
  }

  ABSL_ASSERT(object->type() == Object::kBlob);
  auto blob = object->ReleaseAs<Blob>();
  auto blob_handles = absl::MakeSpan(blob->handles());
#if BUILDFLAG(IS_WIN)
  uint8_t* blob_data =
      reinterpret_cast<uint8_t*>(handle_data + blob_handles.size());
#else
  uint8_t* blob_data = reinterpret_cast<uint8_t*>(&header + 1);
#endif
  memcpy(blob_data, blob->message().data(), blob->message().size());
  header.size = static_cast<uint32_t>(blob->message().size());
  for (size_t i = 0; i < blob_handles.size(); ++i) {
#if BUILDFLAG(IS_WIN)
    handle_data[i] = TransferHandle(blob_handles[i].ReleaseHandle(),
                                    current_process, remote_process);
#else
    handles[i] = Object::ReleaseAsHandle(
        MakeRefCounted<WrappedOSHandle>(std::move(blob_handles[i])));
#endif
  }
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_API
SerializeWithForcedObjectBrokering(IpczDriverHandle handle,
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

  if (object->type() != Object::kTransport &&
      object->type() != Object::kMemory && object->type() != Object::kBlob) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (transport == IPCZ_INVALID_DRIVER_HANDLE) {
    return IPCZ_RESULT_ABORTED;
  }

  MultiprocessTransport& t = MultiprocessTransport::FromHandle(transport);
  if (t.source() != MultiprocessTransportSource::kFromBroker &&
      t.target() != MultiprocessTransportTarget::kToBroker) {
    // For this driver, direct object transmission is supported only when going
    // to or from a broker node.
    return IPCZ_RESULT_PERMISSION_DENIED;
  }

  return Serialize(handle, transport, flags, options, data, num_bytes, handles,
                   num_handles);
}

IpczResult IPCZ_API Deserialize(const uint8_t* data,
                                uint32_t num_bytes,
                                const IpczDriverHandle* handles,
                                uint32_t num_handles,
                                IpczDriverHandle transport,
                                uint32_t flags,
                                const void* options,
                                IpczDriverHandle* driver_handle) {
  if (num_bytes < sizeof(SerializedObject)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  const SerializedObject& header =
      *reinterpret_cast<const SerializedObject*>(data);

#if BUILDFLAG(IS_WIN)
  // Handles on Windows are always transported as data by this driver.
  ABSL_ASSERT(num_handles == 0);
  const OSProcess current_process = OSProcess::GetCurrent();
  const OSProcess& remote_process =
      MultiprocessTransport::FromHandle(transport).remote_process();
#endif

  if (header.type == Object::kBlob) {
    if (sizeof(header) + header.size > num_bytes) {
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

#if BUILDFLAG(IS_WIN)
    const HANDLE* handle_data = reinterpret_cast<const HANDLE*>(
        reinterpret_cast<const uint8_t*>(&header + 1));
    num_handles = (num_bytes - sizeof(header) - header.size) / sizeof(HANDLE);
#endif

    std::vector<OSHandle> os_handles(num_handles);
    for (size_t i = 0; i < num_handles; ++i) {
#if BUILDFLAG(IS_WIN)
      os_handles[i] = OSHandle(
          TransferHandle(handle_data[i], remote_process, current_process));
#else
      os_handles[i] = Object::ReleaseFromHandleAs<WrappedOSHandle>(handles[i])
                          ->TakeHandle();
#endif
    }

#if BUILDFLAG(IS_WIN)
    const char* string_data =
        reinterpret_cast<const char*>(handle_data + num_handles);
#else
    const char* string_data = reinterpret_cast<const char*>(&header + 1);
#endif
    Ref<Blob> blob = MakeRefCounted<Blob>(
        std::string_view(string_data, header.size), absl::MakeSpan(os_handles));
    *driver_handle = ToDriverHandle(blob.release());
    return IPCZ_RESULT_OK;
  }

#if BUILDFLAG(IS_WIN)
  if (num_bytes != sizeof(SerializedObject) + sizeof(HANDLE)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  const HANDLE* handle_data = reinterpret_cast<const HANDLE*>(&header + 1);
  OSHandle handle =
      OSHandle(TransferHandle(handle_data[0], remote_process, current_process));
#else
  if (num_bytes != sizeof(SerializedObject) || num_handles != 1) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  OSHandle handle =
      Object::ReleaseFromHandleAs<WrappedOSHandle>(handles[0])->TakeHandle();
#endif

  if (!handle.is_valid()) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (header.type == Object::kTransport) {
    Channel channel(std::move(handle));
    if (!channel.is_valid()) {
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    auto new_transport = MakeRefCounted<MultiprocessTransport>(
        std::move(channel), OSProcess(), header.source, header.target);
    *driver_handle = ToDriverHandle(new_transport.release());
    return IPCZ_RESULT_OK;
  }

  if (header.type == Object::kMemory) {
    auto memory =
        MakeRefCounted<MultiprocessMemory>(std::move(handle), header.size);
    *driver_handle = ToDriverHandle(memory.release());
    return IPCZ_RESULT_OK;
  }

  return IPCZ_RESULT_INVALID_ARGUMENT;
}

IpczResult IPCZ_API CreateTransports(IpczDriverHandle transport0,
                                     IpczDriverHandle transport1,
                                     uint32_t flags,
                                     const void* options,
                                     IpczDriverHandle* new_transport0,
                                     IpczDriverHandle* new_transport1) {
  using Source = MultiprocessTransportSource;
  using Target = MultiprocessTransportTarget;
  const Target target0 = MultiprocessTransport::FromHandle(transport0).target();
  const Target target1 = MultiprocessTransport::FromHandle(transport1).target();
  const Source source0 = target1 == Target::kToBroker ? Source::kFromBroker
                                                      : Source::kFromNonBroker;
  const Source source1 = target0 == Target::kToBroker ? Source::kFromBroker
                                                      : Source::kFromNonBroker;
  auto [first_channel, second_channel] = Channel::CreateChannelPair();
  auto first = MakeRefCounted<MultiprocessTransport>(
      std::move(first_channel), OSProcess(), source0, target0);
  auto second = MakeRefCounted<MultiprocessTransport>(
      std::move(second_channel), OSProcess(), source1, target1);
  *new_transport0 = ToDriverHandle(first.release());
  *new_transport1 = ToDriverHandle(second.release());
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_API
ActivateTransport(IpczDriverHandle driver_transport,
                  IpczHandle transport,
                  IpczTransportActivityHandler activity_handler,
                  uint32_t flags,
                  const void* options) {
  ToRef<MultiprocessTransport>(driver_transport)
      .Activate(transport, activity_handler);
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_API DeactivateTransport(IpczDriverHandle driver_transport,
                                        uint32_t flags,
                                        const void* options) {
  ToRef<MultiprocessTransport>(driver_transport).Deactivate();
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_API Transmit(IpczDriverHandle driver_transport,
                             const uint8_t* data,
                             uint32_t num_bytes,
                             const IpczDriverHandle* handles,
                             uint32_t num_handles,
                             uint32_t flags,
                             const void* options) {
  return ToRef<MultiprocessTransport>(driver_transport)
      .Transmit(absl::MakeSpan(data, num_bytes),
                absl::MakeSpan(handles, num_handles));
}

IpczResult IPCZ_API AllocateSharedMemory(uint32_t num_bytes,
                                         uint32_t flags,
                                         const void* options,
                                         IpczDriverHandle* driver_memory) {
  auto memory = MakeRefCounted<MultiprocessMemory>(num_bytes);
  *driver_memory = ToDriverHandle(memory.release());
  return IPCZ_RESULT_OK;
}

IpczResult IPCZ_API DuplicateSharedMemory(IpczDriverHandle driver_memory,
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

IpczResult IPCZ_API MapSharedMemory(IpczDriverHandle driver_memory,
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

const IpczDriver kMultiprocessReferenceDriverWithForcedObjectBrokering = {
    sizeof(kMultiprocessReferenceDriver),
    Close,
    SerializeWithForcedObjectBrokering,
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

IpczDriverHandle CreateTransportFromChannel(
    Channel channel,
    OSProcess remote_process,
    MultiprocessTransportSource source,
    MultiprocessTransportTarget target) {
  auto transport = MakeRefCounted<MultiprocessTransport>(
      std::move(channel), std::move(remote_process), source, target);
  return ToDriverHandle(transport.release());
}

}  // namespace ipcz::reference_drivers
