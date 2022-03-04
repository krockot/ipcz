// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_REFERENCE_DRIVERS_WRAPPED_OS_HANDLE_H_
#define IPCZ_SRC_REFERENCE_DRIVERS_WRAPPED_OS_HANDLE_H_

#include "reference_drivers/object.h"
#include "reference_drivers/os_handle.h"

namespace ipcz::reference_drivers {

// Wraps an OSHandle as a driver object. The multiprocess reference driver uses
// this to facilitate serialization of more complex objects into transmissible
// handles on some platforms.
class WrappedOSHandle : public Object {
 public:
  explicit WrappedOSHandle(OSHandle handle);

  const OSHandle& handle() const { return handle_; }
  OSHandle TakeHandle() { return std::move(handle_); }

  // Object:
  IpczResult Close() override;

 private:
  ~WrappedOSHandle() override;

  OSHandle handle_;
};

}  // namespace ipcz::reference_drivers

#endif  // IPCZ_SRC_REFERENCE_DRIVERS_WRAPPED_OS_HANDLE_H_
