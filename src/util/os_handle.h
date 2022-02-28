// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_UTIL_OS_HANDLE_H_
#define IPCZ_SRC_UTIL_OS_HANDLE_H_

#include <algorithm>

#include "build/build_config.h"
#include "ipcz/ipcz.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#elif BUILDFLAG(IS_FUCHSIA)
#include <lib/zx/handle.h>
#elif BUILDFLAG(IS_MAC)
#include <mach/mach.h>
#endif

namespace ipcz {

// Generic scoper to wrap various types of platform-specific OS handles.
// Depending on target platform, an OSHandle may be a Windows HANDLE, a POSIX
// fie descriptor, a Fuchsia handle, or a Mach send or receive right.
class OSHandle {
 public:
  enum class Type {
    kInvalid,
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_FUCHSIA)
    kHandle,
#elif BUILDFLAG(IS_MAC)
    kMachSendRight,
    kMachReceiveRight,
#endif
#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
    kFileDescriptor,
#endif
  };

  OSHandle();

#if BUILDFLAG(IS_WIN)
  explicit OSHandle(HANDLE handle);
#elif BUILDFLAG(IS_FUCHSIA)
  explicit OSHandle(zx::handle handle);
#elif BUILDFLAG(IS_MAC)
  OSHandle(mach_port_t port, Type type);
#endif

#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
  explicit OSHandle(int fd);
#endif

  OSHandle(const OSHandle&) = delete;
  OSHandle& operator=(const OSHandle&) = delete;

  OSHandle(OSHandle&& other);
  OSHandle& operator=(OSHandle&& other);

  ~OSHandle();

  static bool ToIpczOSHandle(OSHandle handle, IpczOSHandle* os_handle);
  static OSHandle FromIpczOSHandle(const IpczOSHandle& os_handle);

  Type type() const { return type_; }

  void reset();

  // Relinquishes ownership of the underlying handle, regardless of type, and
  // discards its value. To release and obtain the underlying handle value, use
  // one of the specific |Release*()| methods below.
  void release();

  // Duplicates the underlying OS handle, returning a new OSHandle which owns
  // it.
  OSHandle Clone() const;

#if BUILDFLAG(IS_WIN)
  bool is_valid() const { return is_valid_handle(); }
  bool is_valid_handle() const { return handle_ != INVALID_HANDLE_VALUE; }
  bool is_handle() const { return type_ == Type::kHandle; }
  HANDLE handle() const { return handle_; }
  HANDLE ReleaseHandle() {
    type_ = Type::kInvalid;
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::swap(handle, handle_);
    return handle;
  }
#elif BUILDFLAG(IS_FUCHSIA)
  bool is_valid() const { return is_valid_fd() || is_valid_handle(); }
  bool is_valid_handle() const { return handle_.is_valid(); }
  bool is_handle() const { return type_ == Type::kHandle; }
  const zx::handle& handle() const { return handle_; }
  zx::handle TakeHandle() {
    if (type_ == Type::kHandle) {
      type_ = Type::kInvalid;
    }
    return std::move(handle_);
  }
  zx_handle_t ReleaseHandle() {
    if (type_ == Type::kHandle) {
      type_ = Type::kInvalid;
    }
    return handle_.release();
  }
#elif BUILDFLAG(IS_MAC)
  bool is_valid() const { return is_valid_fd() || is_valid_mach_port(); }
  bool is_valid_mach_port() const {
    return is_valid_mach_send() || is_valid_mach_receive();
  }

  bool is_valid_mach_send_right() const {
    return mach_send_right_ != MACH_PORT_NULL;
  }
  bool is_mach_send_right() const { return type_ == Type::kMachSendRight; }
  mach_port_t mach_send_right() const { return mach_send_right_; }
  mach_port_t ReleaseMachSendRight() {
    if (type_ == Type::kMachSendRight) {
      type_ = Type::kInvalid;
    }
    return mach_send_right_;
  }

  bool is_valid_mach_receive_right() const {
    return mach_receive_right_ != MACH_PORT_NULL;
  }
  bool is_mach_receive_right() const {
    return type_ == Type::kMachReceiveRight;
  }
  mach_port_t mach_receive_right() const { return mach_receive_right_; }
  mach_port_t ReleaseMachReceiveRight() {
    if (type_ == Type::kMachReceiveRight) {
      type_ = Type::kInvalid;
    }
    return mach_receive_right_;
  }
#elif BUILDFLAG(IS_POSIX)
  bool is_valid() const { return is_valid_fd(); }
#endif

#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
  bool is_valid_fd() const { return fd_ != -1; }
  bool is_fd() const { return type_ == Type::kFileDescriptor; }
  int fd() const { return fd_; }
  int ReleaseFD() {
    if (type_ == Type::kFileDescriptor) {
      type_ = Type::kInvalid;
    }
    return fd_;
  }
#endif

 private:
  Type type_ = Type::kInvalid;
#if BUILDFLAG(IS_WIN)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#elif BUILDFLAG(IS_FUCHSIA)
  zx::handle handle_;
#elif BUILDFLAG(IS_MAC)
  mach_port_t mach_send_right_ = MACH_PORT_NULL;
  mach_port_t mach_receive_right_ = MACH_PORT_NULL;
#endif

#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
  int fd_ = -1;
#endif
};

}  // namespace ipcz

#endif  // IPCZ_SRC_UTIL_OS_HANDLE_H_
