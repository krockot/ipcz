// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "os/process.h"

#include <algorithm>

#include "build/build_config.h"

namespace ipcz {
namespace os {

Process::Process() = default;

Process::Process(ProcessHandle handle) {
#if defined(OS_WIN)
  if (handle == ::GetCurrentProcess()) {
    is_current_process_ = true;
  } else {
    handle_ = handle;
  }
#elif defined(OS_FUCHSIA)
  if (handle == zx_process_self()) {
    is_current_process_ = true;
  } else {
    process_ = zx::process(handle);
  }
#elif defined(OS_POSIX)
  handle_ = handle;
#endif
}

Process::Process(Process&& other) {
#if defined(OS_WIN)
  std::swap(other.handle_, handle_);
#elif defined(OS_FUCHSIA)
  process_ = std::move(other.process_);
  std::swap(other.is_current_process_, is_current_process_);
#elif defined(OS_POSIX)
  std::swap(other.handle_, handle_);
#endif
}

Process& Process::operator=(Process&& other) {
  reset();
#if defined(OS_WIN)
  std::swap(other.handle_, handle_);
#elif defined(OS_FUCHSIA)
  process_ = std::move(other.process_);
  std::swap(other.is_current_process_, is_current_process_);
#elif defined(OS_POSIX)
  std::swap(other.handle_, handle_);
#endif
  return *this;
}

Process::~Process() {
  reset();
}

// static
Process Process::FromIpczOSProcessHandle(const IpczOSProcessHandle& handle) {
#if defined(OS_WIN)
  return Process(
      reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle.value)));
#else
  return Process(static_cast<ProcessHandle>(handle.value));
#endif
}

void Process::reset() {
#if defined(OS_WIN)
  is_current_process_ = false;
  if (handle_ != INVALID_HANDLE_VALUE) {
    ::CloseHandle(handle_);
  }
  handle_ = INVALID_HANDLE_VALUE;
#elif defined(OS_FUCHSIA)
  is_current_process_ = false;
  process_.reset();
#elif defined(OS_POSIX)
  handle_ = kNullProcessHandle;
#endif
}

}  // namespace os
}  // namespace ipcz
