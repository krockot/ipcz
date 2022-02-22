// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_HANDLE_DESCRIPTOR_H_
#define IPCZ_SRC_IPCZ_HANDLE_DESCRIPTOR_H_

#include "ipcz/ipcz.h"

namespace ipcz {

// Serialized representation of an IpczHandle within a parcel.
struct IPCZ_ALIGN(8) HandleDescriptor {
  enum Type : uint32_t {
    // A portal handle consumes the next available RouterDescriptor in the
    // parcel. It does not consume any other data, or any OS handles.
    kPortal = 0,

    // A box handle consumes a driver-defined number of bytes and OS handles
    // from the message that carries it. The amount consumed is serialized in
    // the `num_bytes` and `num_os_handles` fields below.
    //
    // Bytes and OS handles are consumed sequentially from a aggregate fields in
    // the AcceptParcel message; namely `handle_data` and `os_handles` array
    // fields.
    kBox = 1,
  };

  Type type;

  // The number of bytes of handle data to consume for this handle.
  uint32_t num_bytes;

  // The number of OS handles to consume for this handle.
  uint32_t num_os_handles;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_HANDLE_DESCRIPTOR_H_
