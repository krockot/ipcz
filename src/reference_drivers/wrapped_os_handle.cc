// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "reference_drivers/wrapped_os_handle.h"

namespace ipcz::reference_drivers {

WrappedOSHandle::WrappedOSHandle(OSHandle handle)
    : handle_(std::move(handle)) {}

WrappedOSHandle::~WrappedOSHandle() = default;

IpczResult WrappedOSHandle::Close() {
  handle_.reset();
  return IPCZ_RESULT_OK;
}

}  // namespace ipcz::reference_drivers

