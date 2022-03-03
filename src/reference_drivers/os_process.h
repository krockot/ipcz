// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_REFERENCE_DRIVERS_OS_PROCESS_H_
#define IPCZ_SRC_REFERENCE_DRIVERS_OS_PROCESS_H_

#include <cstdint>

#include <sys/types.h>

#include "build/build_config.h"
#include "reference_drivers/os_handle.h"
#include "reference_drivers/os_process.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#elif BUILDFLAG(IS_FUCHSIA)
#include <zircon/types.h>
#endif

namespace ipcz::reference_drivers {

// Platform-specific, unscoped raw process handle types and constants.
#if BUILDFLAG(IS_WIN)
using ProcessHandle = HANDLE;
using ProcessId = DWORD;
const ProcessHandle kNullProcessHandle = NULL;
const ProcessId kNullProcessId = 0;
#elif BUILDFLAG(IS_FUCHSIA)
using ProcessHandle = zx_handle_t;
using ProcessId = zx_koid_t;
const ProcessHandle kNullProcessHandle = ZX_HANDLE_INVALID;
const ProcessId kNullProcessId = ZX_KOID_INVALID;
#elif BUILDFLAG(IS_POSIX)
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

  static OSProcess GetCurrent();

  bool is_valid() const {
#if BUILDFLAG(IS_FUCHSIA)
    return is_current_process_ || process_.is_valid();
#elif BUILDFLAG(IS_WIN)
    return is_current_process_ || handle_ != kNullProcessHandle;
#else
    return handle_ != kNullProcessHandle;
#endif
  }

  ProcessHandle handle() const {
#if BUILDFLAG(IS_FUCHSIA)
    if (is_current_process_) {
      return zx_process_self();
    }
    return process_.get();
#elif BUILDFLAG(IS_WIN)
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
#if BUILDFLAG(IS_FUCHSIA)
  zx::process process_;
#else
  ProcessHandle handle_ = kNullProcessHandle;
#endif

#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_FUCHSIA)
  bool is_current_process_ = false;
#endif
};

}  // namespace ipcz::reference_drivers

#endif  // IPCZ_SRC_REFERENCE_DRIVERS_OS_PROCESS_H_
