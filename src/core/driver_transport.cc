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

DriverTransport::~DriverTransport() = default;

IpczResult DriverTransport::Activate() {
  // Acquire a self-reference, balanced in NotifyTransport() when the driver
  // invokes its activity handler with IPCZ_TRANSPORT_ACTIVITY_DEACTIVATED.
  IpczHandle handle = ToHandle(mem::WrapRefCounted(this).release());
  return driver_.ActivateTransport(driver_transport_, handle, NotifyTransport,
                                   IPCZ_NO_FLAGS, nullptr);
}

IpczResult DriverTransport::Deactivate() {
  return driver_.DestroyTransport(driver_transport_, IPCZ_NO_FLAGS, nullptr);
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
