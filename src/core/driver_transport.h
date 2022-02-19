// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_DRIVER_TRANSPORT_H_
#define IPCZ_SRC_CORE_DRIVER_TRANSPORT_H_

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

// Encapsulates shared ownership of a transport endpoint created by an ipcz
// driver..
class DriverTransport : public mem::RefCounted {
 public:
  using Pair = std::pair<mem::Ref<DriverTransport>, mem::Ref<DriverTransport>>;

  struct Descriptor {
    Descriptor();
    Descriptor(Descriptor&&);
    Descriptor& operator=(Descriptor&&);
    ~Descriptor();

    std::vector<uint8_t> data;
    std::vector<os::Handle> handles;
  };

  class Data : public absl::Span<const uint8_t> {
   public:
    Data();
    Data(absl::Span<const uint8_t> data);
    Data(absl::Span<const char> str);

    template <size_t N>
    Data(const char str[N]) : Data(absl::MakeSpan(str)) {}

    absl::Span<const char> AsString() const;
  };

  struct Message {
    Message(Data data);
    Message(Data data, absl::Span<os::Handle> handles);
    Message(const Message&);
    Message& operator=(const Message&);
    ~Message();

    Data data;
    absl::Span<os::Handle> handles;
  };

  class Listener {
   public:
    virtual ~Listener() = default;

    // Accepts a raw message from the transport. The ONLY validation done before
    // calling this is to ensure that the message data is at least long enough
    // to contain a complete internal::MessageHeader, and that the header's
    // purported length (the `size` field) is at least that large as well.
    virtual IpczResult OnTransportMessage(const Message& message) = 0;

    virtual void OnTransportError() = 0;
  };

  DriverTransport(const IpczDriver& driver, IpczDriverHandle driver_transport);

  static DriverTransport::Pair CreatePair(const IpczDriver& driver,
                                          IpczDriverHandle driver_node);

  // Set the object handling any incoming message or error notifications. This
  // is only safe to set before Activate() is called, or from within one of the
  // Listener methods when invoked by this DriverTransport (because invocations
  // are mutually exclusive). `listener` must outlive this DriverTransport.
  void set_listener(Listener* listener) { listener_ = listener; }

  // Releases the driver handle so that it's no longer controlled by this
  // DriverTranport.
  IpczDriverHandle Release();

  // Begins listening on the transport for incoming data and OS handles. Once
  // this is called, the transport's Listener may be invoked by the driver at
  // any time from arbitrary threads. The driver will continue listening until
  // Deactivate() is called.
  IpczResult Activate();

  // Requests that the driver cease listening for incoming data and OS handles
  // on this transport. Once a transport is deactivated, it can never be
  // reactivated.
  IpczResult Deactivate();

  // Asks the driver to submit the data and/or OS handles in `message` for
  // transmission from this transport endpoint to the corresponding opposite
  // endpoint.
  IpczResult TransmitMessage(const Message& message);

  // Serializes this transport into a collection of `data` and `handles` for
  // transmission over some other transport. A transport must not be serialized
  // once it has been activated.
  IpczResult Serialize(std::vector<uint8_t>& data,
                       std::vector<os::Handle>& handles);

  // Deserializes a new transport from data and OS handles previously serialized
  // by Serialize() above.
  static mem::Ref<DriverTransport> Deserialize(const IpczDriver& driver,
                                               IpczDriverHandle driver_node,
                                               absl::Span<const uint8_t> data,
                                               absl::Span<os::Handle> handles);

  // Transmits a Node message over this transport.
  template <typename T>
  IpczResult Transmit(T& message) {
    message.Serialize(T::kMetadata);
    return TransmitMessage(
        Message(Data(message.data_view()), message.handles_view()));
  }

  // Invoked by the driver any time this transport receives data and/or OS
  // handles to be passed back into ipcz.
  IpczResult Notify(const Message& message);
  void NotifyError();

 private:
  ~DriverTransport() override;

  const IpczDriver& driver_;
  IpczDriverHandle driver_transport_;

  bool serialized_ = false;
  Listener* listener_ = nullptr;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_DRIVER_TRANSPORT_H_
