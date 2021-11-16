// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_TRANSPORT_H_
#define IPCZ_SRC_CORE_TRANSPORT_H_

#include <cstdint>
#include <vector>

#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class Node;
class NodeLink;

// Encapsulates a transport to carry data and handles from one node to another.
// All actual I/O is delegated to the IpczDriver, and this object only acts as
// a bridge between the driver and the corresponding NodeLink which uses it for
// I/O.
class Transport : public mem::RefCounted {
 public:
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

  Transport(mem::Ref<Node> node, IpczDriverHandle driver_transport);

  IpczResult Activate(mem::Ref<NodeLink> for_link);
  IpczResult Deactivate();

  IpczResult Transmit(const Message& message);
  void Notify(const Message& message);
  void NotifyError();

 private:
  ~Transport() override;

  const mem::Ref<Node> node_;
  const IpczDriver& driver_;
  const IpczDriverHandle driver_transport_;

  mem::Ref<NodeLink> node_link_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_TRANSPORT_H_
