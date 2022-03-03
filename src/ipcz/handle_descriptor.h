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

    // A box handle consumes the next available deserialized DriverObject in the
    // parcel.
    kBox = 1,
  };

  Type type;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_HANDLE_DESCRIPTOR_H_
