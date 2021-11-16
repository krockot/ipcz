// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_UTIL_HANDLE_UTIL_H_
#define IPCZ_SRC_UTIL_HANDLE_UTIL_H_

#include <cstddef>

#include "ipcz/ipcz.h"

namespace ipcz {

template <typename T>
IpczHandle ToHandle(T* ptr) {
  return static_cast<IpczHandle>(reinterpret_cast<uintptr_t>(ptr));
}

template <typename T>
IpczDriverHandle ToDriverHandle(T* ptr) {
  return static_cast<IpczDriverHandle>(reinterpret_cast<uintptr_t>(ptr));
}

template <typename T, typename HandleType>
T* ToPtr(HandleType handle) {
  return reinterpret_cast<T*>(static_cast<uintptr_t>(handle));
}

template <typename T, typename HandleType>
T& ToRef(HandleType handle) {
  return *ToPtr<T>(handle);
}

}  // namespace ipcz

#endif  // IPCZ_SRC_UTIL_HANDLE_UTIL_H_
