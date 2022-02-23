// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_UTIL_OS_PROCESS_H_
#define IPCZ_SRC_UTIL_OS_PROCESS_H_

#include <cstdint>

#include <sys/types.h>

#include "build/build_config.h"
#include "ipcz/ipcz.h"
#include "util/os_handle.h"

#if defined(OS_WIN)
#include <windows.h>
#elif defined(OS_FUCHSIA)
#include <zircon/types.h>
#endif

namespace ipcz {

// Platform-specific, unscoped raw process handle types and constants.
#if defined(OS_WIN)
using ProcessHandle = HANDLE;
using ProcessId = DWORD;
const ProcessHandle kNullProcessHandle = NULL;
const ProcessId kNullProcessId = 0;
#elif defined(OS_FUCHSIA)
using ProcessHandle = zx_handle_t;
using ProcessId = zx_koid_t;
const ProcessHandle kNullProcessHandle = ZX_HANDLE_INVALID;
const ProcessId kNullProcessId = ZX_KOID_INVALID;
#elif defined(OS_POSIX)
using ProcessHandle = pid_t;
using ProcessId = pid_t;
const ProcessHandle kNullProcessHandle = 0;
const ProcessId kNullProcessId = 0;
#endif

// A cross-platform abstraction around a process handle. This models strong
// unique ownership of a handle even for platforms where it it's unnecessary
// (e.g. PIDs on POSIX systems).
class OSProcess {
 public:
  OSProcess();
  explicit OSProcess(ProcessHandle handle);
  OSProcess(OSProcess&& other);
  OSProcess& operator=(OSProcess&& other);
  OSProcess(const OSProcess&) = delete;
  OSProcess& operator=(const OSProcess&) = delete;
  ~OSProcess();

  static OSProcess FromIpczOSProcessHandle(const IpczOSProcessHandle& handle);
  static bool ToIpczOSProcessHandle(OSProcess procss, IpczOSProcessHandle& out);

  static OSProcess GetCurrent();

  bool is_valid() const {
#if defined(OS_FUCHSIA)
    return is_current_process_ || process_.is_valid();
#elif defined(OS_WIN)
    return is_current_process_ || handle_ != kNullProcessHandle;
#else
    return handle_ != kNullProcessHandle;
#endif
  }

  ProcessHandle handle() const {
#if defined(OS_FUCHSIA)
    if (is_current_process_) {
      return zx_process_self();
    }
    return process_.get();
#elif defined(OS_WIN)
    if (is_current_process_) {
      return ::GetCurrentProcess();
    }
    return handle_;
#else
    return handle_;
#endif
  }

  void reset();

  OSProcess Clone() const;

  OSHandle TakeAsHandle();

 private:
#if defined(OS_FUCHSIA)
  zx::process process_;
#else
  ProcessHandle handle_ = kNullProcessHandle;
#endif

#if defined(OS_WIN) || defined(OS_FUCHSIA)
  bool is_current_process_ = false;
#endif
};

}  // namespace ipcz

#endif  // IPCZ_SRC_UTIL_OS_PROCESS_H_