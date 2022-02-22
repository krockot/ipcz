// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_DRIVER_OBJECT_H_
#define IPCZ_SRC_IPCZ_DRIVER_OBJECT_H_

#include <cstdint>

#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/os_handle.h"
#include "util/ref_counted.h"

namespace ipcz {

class Node;

// Owns an IpczDriverHandle and exposes a generic interface for serialization
// and deserialization through the driver.
class DriverObject {
 public:
  DriverObject();
  DriverObject(Ref<Node> node, IpczDriverHandle handle);
  DriverObject(DriverObject&&);
  DriverObject& operator=(DriverObject&&);
  ~DriverObject();

  const Ref<Node>& node() const { return node_; }
  IpczDriverHandle handle() const { return handle_; }

  void reset();
  IpczDriverHandle release();

  bool is_valid() const { return handle_ != IPCZ_INVALID_DRIVER_HANDLE; }

  bool IsSerializable() const;

  struct SerializedDimensions {
    uint32_t num_bytes;
    uint32_t num_os_handles;
  };
  SerializedDimensions GetSerializedDimensions() const;

  // Serializes this object into `data` and `handles`, which must both be at
  // least large enough to support the object's serialized dimensions. If short,
  // returns IPCZ_RESULT_RESOURCE_EXHAUSTED.
  //
  // May also return IPCZ_RESULT_FAILED_PRECONDITION if the object is not
  // currently in a serializable state, or IPCZ_RESULT_DATA_LOSS if the driver
  // cannot serialize this type of object.
  IpczResult Serialize(absl::Span<uint8_t> data, absl::Span<OSHandle> handles);

  // Asks the node to deserialize a driver object from a series of bytes and
  // OS handles produced by a prior call to Serialize(). Returns an opaque
  // IpczDriverHandle on success or IPCZ_INVALID_DRIVER_HANDLE on failure.
  static DriverObject Deserialize(Ref<Node> node,
                                  absl::Span<const uint8_t> data,
                                  absl::Span<OSHandle> handles);

 private:
  Ref<Node> node_;
  IpczDriverHandle handle_ = IPCZ_INVALID_DRIVER_HANDLE;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_DRIVER_OBJECT_H_
