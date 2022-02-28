// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/os_process.h"

#include <algorithm>

#if BUILDFLAG(IS_POSIX)
#include <sys/types.h>
#include <unistd.h>
#endif

#include "build/build_config.h"

namespace ipcz {

OSProcess::OSProcess() = default;

OSProcess::OSProcess(ProcessHandle handle) {
#if BUILDFLAG(IS_WIN)
  if (handle == ::GetCurrentProcess()) {
    is_current_process_ = true;
  } else {
    handle_ = handle;
  }
#elif BUILDFLAG(IS_FUCHSIA)
  if (handle == zx_process_self()) {
    is_current_process_ = true;
  } else {
    process_ = zx::process(handle);
  }
#elif BUILDFLAG(IS_POSIX)
  handle_ = handle;
#endif
}

OSProcess::OSProcess(OSProcess&& other) {
#if BUILDFLAG(IS_WIN)
  std::swap(other.handle_, handle_);
#elif BUILDFLAG(IS_FUCHSIA)
  process_ = std::move(other.process_);
  std::swap(other.is_current_process_, is_current_process_);
#elif BUILDFLAG(IS_POSIX)
  std::swap(other.handle_, handle_);
#endif
}

OSProcess& OSProcess::operator=(OSProcess&& other) {
  reset();
#if BUILDFLAG(IS_WIN)
  std::swap(other.handle_, handle_);
#elif BUILDFLAG(IS_FUCHSIA)
  process_ = std::move(other.process_);
  std::swap(other.is_current_process_, is_current_process_);
#elif BUILDFLAG(IS_POSIX)
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
#if BUILDFLAG(IS_WIN)
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
#if BUILDFLAG(IS_WIN)
  OSHandle handle = process.TakeAsHandle();
  out.value = static_cast<uint64_t>(
      reinterpret_cast<uintptr_t>(handle.ReleaseHandle()));
#elif BUILDFLAG(IS_FUCHSIA)
  out.value = reinterpret_cast<uint64_t>(process.process_);
#else
  out.value = static_cast<uint64_t>(process.handle_);
#endif
  return true;
}

// static
OSProcess OSProcess::GetCurrent() {
#if BUILDFLAG(IS_WIN)
  return OSProcess(::GetCurrentProcess());
#elif BUILDFLAG(IS_FUCHSIA)
  return OSProcess(zx_process_self());
#elif BUILDFLAG(IS_POSIX)
  return OSProcess(getpid());
#endif
}

void OSProcess::reset() {
#if BUILDFLAG(IS_WIN)
  is_current_process_ = false;
  if (handle_ != INVALID_HANDLE_VALUE) {
    ::CloseHandle(handle_);
  }
  handle_ = INVALID_HANDLE_VALUE;
#elif BUILDFLAG(IS_FUCHSIA)
  is_current_process_ = false;
  process_.reset();
#elif BUILDFLAG(IS_POSIX)
  handle_ = kNullProcessHandle;
#endif
}

OSProcess OSProcess::Clone() const {
  OSProcess clone;
#if BUILDFLAG(IS_WIN)
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
#elif BUILDFLAG(IS_FUCHSIA)
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
#elif BUILDFLAG(IS_POSIX)
  clone.handle_ = handle_;
#endif
  return clone;
}

OSHandle OSProcess::TakeAsHandle() {
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_FUCHSIA)
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
