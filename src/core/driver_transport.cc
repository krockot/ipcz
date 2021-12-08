#include "core/driver_transport.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>

#include "build/build_config.h"
#include "core/message_internal.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/handle_util.h"

#if defined(OS_WIN)
#define CDECL __cdecl
#else
#define CDECL
#endif

namespace ipcz {
namespace core {

namespace {

IpczResult CDECL NotifyTransport(IpczHandle transport,
                                 const uint8_t* data,
                                 uint32_t num_bytes,
                                 const struct IpczOSHandle* os_handles,
                                 uint32_t num_os_handles,
                                 IpczTransportActivityFlags flags,
                                 const void* options) {
  if (transport == IPCZ_INVALID_HANDLE) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (flags & IPCZ_TRANSPORT_ACTIVITY_DEACTIVATED) {
    mem::Ref<DriverTransport> doomed_transport(
        mem::RefCounted::kAdoptExistingRef, ToPtr<DriverTransport>(transport));
    return IPCZ_RESULT_OK;
  }

  DriverTransport& t = ToRef<DriverTransport>(transport);
  if (flags & IPCZ_TRANSPORT_ACTIVITY_ERROR) {
    t.NotifyError();
    return IPCZ_RESULT_OK;
  }

  absl::InlinedVector<os::Handle, 2> handles;
  handles.resize(num_os_handles);
  for (size_t i = 0; i < num_os_handles; ++i) {
    handles[i] = os::Handle::FromIpczOSHandle(os_handles[i]);
  }

  return t.Notify(DriverTransport::Message(
      absl::Span<const uint8_t>(data, num_bytes), absl::MakeSpan(handles)));
}

}  // namespace

DriverTransport::Descriptor::Descriptor() = default;

DriverTransport::Descriptor::Descriptor(Descriptor&&) = default;

DriverTransport::Descriptor& DriverTransport::Descriptor::operator=(
    Descriptor&&) = default;

DriverTransport::Descriptor::~Descriptor() = default;

DriverTransport::Data::Data() = default;

DriverTransport::Data::Data(absl::Span<const uint8_t> data) : Span(data) {}

DriverTransport::Data::Data(absl::Span<const char> str)
    : Span(reinterpret_cast<const uint8_t*>(str.data()), str.size()) {}

absl::Span<const char> DriverTransport::Data::AsString() const {
  return absl::MakeSpan(reinterpret_cast<const char*>(data()), size());
}

DriverTransport::Message::Message(Data data) : data(data) {}

DriverTransport::Message::Message(Data data, absl::Span<os::Handle> handles)
    : data(data), handles(handles) {}

DriverTransport::Message::Message(const Message&) = default;

DriverTransport::Message& DriverTransport::Message::operator=(const Message&) =
    default;

DriverTransport::Message::~Message() = default;

DriverTransport::DriverTransport(const IpczDriver& driver,
                                 IpczDriverHandle driver_transport)
    : driver_(driver), driver_transport_(driver_transport) {}

DriverTransport::~DriverTransport() {
  if (!serialized_) {
    driver_.DestroyTransport(driver_transport_, IPCZ_NO_FLAGS, nullptr);
  }
}

// static
DriverTransport::Pair DriverTransport::CreatePair(
    const IpczDriver& driver,
    IpczDriverHandle driver_node) {
  IpczDriverHandle transport0;
  IpczDriverHandle transport1;
  IpczResult result = driver.CreateTransports(
      driver_node, IPCZ_NO_FLAGS, nullptr, &transport0, &transport1);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
  auto first = mem::MakeRefCounted<DriverTransport>(driver, transport0);
  auto second = mem::MakeRefCounted<DriverTransport>(driver, transport1);
  return {std::move(first), std::move(second)};
}

IpczResult DriverTransport::Activate() {
  // Acquire a self-reference, balanced in NotifyTransport() when the driver
  // invokes its activity handler with IPCZ_TRANSPORT_ACTIVITY_DEACTIVATED.
  IpczHandle handle = ToHandle(mem::WrapRefCounted(this).release());
  return driver_.ActivateTransport(driver_transport_, handle, NotifyTransport,
                                   IPCZ_NO_FLAGS, nullptr);
}

IpczResult DriverTransport::Deactivate() {
  return driver_.DeactivateTransport(driver_transport_, IPCZ_NO_FLAGS, nullptr);
}

IpczResult DriverTransport::TransmitMessage(const Message& message) {
  absl::InlinedVector<IpczOSHandle, 2> os_handles;
  for (size_t i = 0; i < message.handles.size(); ++i) {
    if (message.handles[i].is_valid()) {
      os_handles.push_back({sizeof(IpczOSHandle)});
      bool ok = os::Handle::ToIpczOSHandle(std::move(message.handles[i]),
                                           &os_handles.back());
      ABSL_ASSERT(ok);
    }
  }

  return driver_.Transmit(
      driver_transport_, message.data.data(),
      static_cast<uint32_t>(message.data.size()), os_handles.data(),
      static_cast<uint32_t>(os_handles.size()), IPCZ_NO_FLAGS, nullptr);
}

IpczResult DriverTransport::Serialize(std::vector<uint8_t>& data,
                                      std::vector<os::Handle>& handles) {
  uint32_t num_bytes = 0;
  uint32_t num_os_handles = 0;
  IpczResult result =
      driver_.SerializeTransport(driver_transport_, IPCZ_NO_FLAGS, nullptr,
                                 nullptr, &num_bytes, nullptr, &num_os_handles);
  ABSL_ASSERT(result == IPCZ_RESULT_RESOURCE_EXHAUSTED);
  data.resize(num_bytes);
  std::vector<IpczOSHandle> os_handles(num_os_handles);
  result = driver_.SerializeTransport(driver_transport_, IPCZ_NO_FLAGS, nullptr,
                                      data.data(), &num_bytes,
                                      os_handles.data(), &num_os_handles);
  ABSL_ASSERT(result != IPCZ_RESULT_RESOURCE_EXHAUSTED);
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  serialized_ = true;

  handles.resize(num_os_handles);
  for (size_t i = 0; i < num_os_handles; ++i) {
    handles[i] = os::Handle::FromIpczOSHandle(os_handles[i]);
  }

  return IPCZ_RESULT_OK;
}

// static
mem::Ref<DriverTransport> DriverTransport::Deserialize(
    const IpczDriver& driver,
    IpczDriverHandle driver_node,
    absl::Span<const uint8_t> data,
    absl::Span<os::Handle> handles) {
  std::vector<IpczOSHandle> os_handles(handles.size());
  bool fail = false;
  for (size_t i = 0; i < handles.size(); ++i) {
    os_handles[i].size = sizeof(os_handles[i]);
    bool ok = os::Handle::ToIpczOSHandle(std::move(handles[i]), &os_handles[i]);
    if (!ok) {
      fail = true;
    }
  }

  if (fail) {
    return nullptr;
  }

  IpczDriverHandle transport;
  IpczResult result = driver.DeserializeTransport(
      driver_node, data.data(), static_cast<uint32_t>(data.size()),
      os_handles.data(), static_cast<uint32_t>(os_handles.size()),
      /*target_process=*/nullptr, IPCZ_NO_FLAGS, nullptr, &transport);
  if (result != IPCZ_RESULT_OK) {
    return nullptr;
  }

  return mem::MakeRefCounted<DriverTransport>(driver, transport);
}

IpczResult DriverTransport::Notify(const Message& message) {
  // Do some basic validation of the header vs the message contents.
  if (message.data.size() < sizeof(internal::MessageHeader)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  const auto& header =
      *reinterpret_cast<const internal::MessageHeader*>(message.data.data());
  if (header.size < sizeof(internal::MessageHeader)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  ABSL_ASSERT(listener_);
  IpczResult result = listener_->OnTransportMessage(message);
  if (result == IPCZ_RESULT_UNIMPLEMENTED && header.expects_reply) {
    internal::MessageHeader nope = {sizeof(nope)};
    nope.message_id = header.message_id;
    nope.request_id = header.request_id;
    nope.wont_reply = true;
    TransmitData(nope);
  }

  return result;
}

void DriverTransport::NotifyError() {
  ABSL_ASSERT(listener_);
  listener_->OnTransportError();
}

}  // namespace core
}  // namespace ipcz
