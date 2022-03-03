// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_DRIVER_TRANSPORT_H_
#define IPCZ_SRC_IPCZ_DRIVER_TRANSPORT_H_

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "ipcz/driver_object.h"
#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/ref_counted.h"

namespace ipcz {

class Node;

// Encapsulates shared ownership of a transport endpoint created by an ipcz
// driver..
class DriverTransport : public RefCounted {
 public:
  using Pair = std::pair<Ref<DriverTransport>, Ref<DriverTransport>>;

  struct Descriptor {
    Descriptor();
    Descriptor(Descriptor&&);
    Descriptor& operator=(Descriptor&&);
    ~Descriptor();

    std::vector<uint8_t> data;
    std::vector<DriverObject> objects;
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
    Message(Data data, absl::Span<const IpczDriverHandle> handles);
    Message(const Message&);
    Message& operator=(const Message&);
    ~Message();

    Data data;
    absl::Span<const IpczDriverHandle> handles;
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

  explicit DriverTransport(DriverObject transport);

  static DriverTransport::Pair CreatePair(Ref<Node> node);

  // Set the object handling any incoming message or error notifications. This
  // is only safe to set before Activate() is called, or from within one of the
  // Listener methods when invoked by this DriverTransport (because invocations
  // are mutually exclusive). `listener` must outlive this DriverTransport.
  void set_listener(Listener* listener) { listener_ = listener; }

  const DriverObject& driver_object() const { return transport_; }

  DriverObject TakeDriverObject() {
    ABSL_ASSERT(!listener_);
    return std::move(transport_);
  }

  // Releases the driver handle so that it's no longer controlled by this
  // DriverTranport.
  IpczDriverHandle Release();

  // Begins listening on the transport for incoming data and driver objects.
  // Once this is called, the transport's Listener may be invoked by the driver
  // at any time from arbitrary threads. The driver will continue listening
  // until Deactivate() is called.
  IpczResult Activate();

  // Requests that the driver cease listening for incoming data and driver
  // objects on this transport. Once a transport is deactivated, it can never be
  // reactivated.
  IpczResult Deactivate();

  // Asks the driver to submit the data and/or driver objects in `message` for
  // transmission from this transport endpoint to the corresponding opposite
  // endpoint.
  IpczResult TransmitMessage(const Message& message);

  // Transmits a Node message over this transport.
  template <typename T>
  IpczResult Transmit(T& message) {
    IpczResult result = message.Serialize(T::kMetadata, *this);
    if (result != IPCZ_RESULT_OK) {
      return result;
    }

    return TransmitMessage(Message(Data(message.data_view()),
                                   message.transmissible_driver_handles()));
  }

  // Invoked by the driver any time this transport receives data and/or driver
  // handles to be passed back into ipcz.
  IpczResult Notify(const Message& message);
  void NotifyError();

 private:
  ~DriverTransport() override;

  DriverObject transport_;

  Listener* listener_ = nullptr;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_DRIVER_TRANSPORT_H_
