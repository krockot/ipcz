// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/os_process.h"

#include <algorithm>

#if defined(OS_POSIX)
#include <sys/types.h>
#include <unistd.h>
#endif

#include "build/build_config.h"

namespace ipcz {

OSProcess::OSProcess() = default;

OSProcess::OSProcess(ProcessHandle handle) {
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

OSProcess::OSProcess(OSProcess&& other) {
#if defined(OS_WIN)
  std::swap(other.handle_, handle_);
#elif defined(OS_FUCHSIA)
  process_ = std::move(other.process_);
  std::swap(other.is_current_process_, is_current_process_);
#elif defined(OS_POSIX)
  std::swap(other.handle_, handle_);
#endif
}

OSProcess& OSProcess::operator=(OSProcess&& other) {
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

OSProcess::~OSProcess() {
  reset();
}

// static
OSProcess OSProcess::FromIpczOSProcessHandle(
    const IpczOSProcessHandle& handle) {
#if defined(OS_WIN)
  return OSProcess(
      reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle.value)));
#else
  return OSProcess(static_cast<ProcessHandle>(handle.value));
#endif
}

// static
bool OSProcess::ToIpczOSProcessHandle(OSProcess process,
                                      IpczOSProcessHandle& out) {
  out.size = sizeof(out);
#if defined(OS_WIN)
  OSHandle handle = process.TakeAsHandle();
  out.value = static_cast<uint64_t>(
      reinterpret_cast<uintptr_t>(handle.ReleaseHandle()));
#elif defined(OS_FUCHA)
  out.value = reinterpret_cast<uint64_t>(process.process_);
#else
  out.value = static_cast<uint64_t>(process.handle_);
#endif
  return true;
}

// static
OSProcess OSProcess::GetCurrent() {
#if defined(OS_WIN)
  return OSProcess(::GetCurrentProcess());
#elif defined(OS_FUCHSIA)
  return OSProcess(zx_process_self());
#elif defined(OS_POSIX)
  return OSProcess(getpid());
#endif
}

void OSProcess::reset() {
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

OSProcess OSProcess::Clone() const {
  OSProcess clone;
#if defined(OS_WIN)
  if (is_current_process_) {
    clone.is_current_process_ = true;
  }
  if (handle_ != INVALID_HANDLE_VALUE) {
    BOOL result =
        ::DuplicateHandle(::GetCurrentProcess(), handle_, ::GetCurrentProcess(),
                          &clone.handle_, 0, FALSE, DUPLICATE_SAME_ACCESS);
    if (!result) {
      return {};
    }
  }
#elif defined(OS_FUCHSIA)
  if (is_current_process_) {
    clone.is_current_process_ = true;
  }

  if (process_.is_valid()) {
    zx_status_t result =
        process_.duplicate(ZX_RIGHT_SAME_RIGHTS, &clone.process_);
    if (result != ZX_OK) {
      return {};
    }
  }
#elif defined(OS_POSIX)
  clone.handle_ = handle_;
#endif
  return clone;
}

OSHandle OSProcess::TakeAsHandle() {
#if defined(OS_WIN) || defined(OS_FUCHSIA)
  ProcessHandle handle = kNullProcessHandle;
  if (is_current_process_) {
    return OSHandle();
  }
  std::swap(handle, handle_);
  return OSHandle(handle);
#else
  return OSHandle();
#endif
}

}  // namespace ipcz
