// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/os_handle.h"

#include <algorithm>

#include "build/build_config.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

#if defined(OS_WIN)
#include <windows.h>
#elif defined(OS_FUCHSIA)
#include <lib/fdio/limits.h>
#include <lib/zx/handle.h>
#include <zircon/status.h>
#elif defined(OS_MAC)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#if defined(OS_POSIX) || defined(OS_FUCHSIA)
#include <errno.h>
#include <unistd.h>
#endif

namespace ipcz {

OSHandle::OSHandle() = default;

#if defined(OS_WIN)
OSHandle::OSHandle(HANDLE handle) : type_(Type::kHandle), handle_(handle) {}
#elif defined(OS_FUCHSIA)
OSHandle::OSHandle(zx::handle handle)
    : type_(Type::kHandle), handle_(std::move(handle)) {}
#elif defined(OS_MAC)
OSHandle::OSHandle(mach_port_t port, Type type) : type_(type) {
  ABSL_ASSERT(type_ == Type::kMachSendRight ||
              type_ == Type::kMachReceiveRight);
  if (type_ == Type::kMachSendRight) {
    mach_send_right_ = port;
  } else {
    mach_receive_right_ = port;
  }
}
#endif

#if defined(OS_POSIX) || defined(OS_FUCHSIA)
OSHandle::OSHandle(int fd) : type_(Type::kFileDescriptor), fd_(fd) {}
#endif

OSHandle::OSHandle(OSHandle&& other) : type_(other.type_) {
#if defined(OS_WIN) || defined(OS_FUCHSIA)
  handle_ = std::move(other.handle_);
#elif defined(OS_MAC)
  mach_send_right_ = other.mach_send_right_;
  mach_receive_right_ = other.mach_receive_right_;
#endif

#if defined(OS_POSIX) || defined(OS_FUCHSIA)
  fd_ = other.fd_;
#endif

  other.type_ = Type::kInvalid;
}

OSHandle& OSHandle::operator=(OSHandle&& other) {
  reset();
  type_ = other.type_;

#if defined(OS_WIN) || defined(OS_FUCHSIA)
  handle_ = std::move(other.handle_);
#elif defined(OS_MAC)
  mach_send_right_ = other.mach_send_right_;
  mach_receive_right_ = other.mach_receive_right_;
#endif

#if defined(OS_POSIX) || defined(OS_FUCHSIA)
  fd_ = other.fd_;
#endif

  other.type_ = Type::kInvalid;
  return *this;
}

OSHandle::~OSHandle() {
  reset();
}

// static
bool OSHandle::ToIpczOSHandle(OSHandle handle, IpczOSHandle* os_handle) {
  ABSL_ASSERT(os_handle);

  if (!handle.is_valid() || os_handle->size < sizeof(IpczOSHandle)) {
    return false;
  }

#if defined(OS_WIN)
  os_handle->type = IPCZ_OS_HANDLE_WINDOWS;
  os_handle->value = static_cast<uint64_t>(
      reinterpret_cast<uintptr_t>(handle.ReleaseHandle()));
#elif defined(OS_FUCHSIA)
  if (handle.is_handle()) {
    os_handle->type = IPCZ_OS_HANDLE_FUCHSIA;
    os_handle->value = static_cast<uint64_t>(handle.ReleaseHandle());
  }
#elif defined(OS_MAC)
  if (handle.is_mach_send_right()) {
    os_handle->type = IPCZ_OS_HANDLE_MACH_SEND_RIGHT;
    os_handle->value = static_cast<uint64_t>(handle.ReleaseMachSendRight());
  } else if (handle.is_mach_receive_right()) {
    os_handle->type = IPCZ_OS_HANDLE_MACH_RECEIVE_RIGHT;
    os_handle->value = static_cast<uint64_t>(handle.ReleaseMachReceiveRight());
  }
#endif

#if defined(OS_POSIX) || defined(OS_FUCHSIA)
  if (handle.is_fd()) {
    os_handle->type = IPCZ_OS_HANDLE_FILE_DESCRIPTOR;
    os_handle->value = static_cast<uint64_t>(handle.ReleaseFD());
  }
#endif
  return true;
}

// static
OSHandle OSHandle::FromIpczOSHandle(const IpczOSHandle& os_handle) {
  switch (os_handle.type) {
#if defined(OS_WIN)
    case IPCZ_OS_HANDLE_WINDOWS: {
      HANDLE handle =
          reinterpret_cast<HANDLE>(static_cast<uintptr_t>(os_handle.value));
      if (handle != INVALID_HANDLE_VALUE) {
        return OSHandle(handle);
      }
      break;
    }
#elif defined(OS_FUCHSIA)
    case IPCZ_OS_HANDLE_FUCHSIA: {
      zx::handle handle(os_handle.value);
      if (handle.is_valid()) {
        return OSHandle(std::move(handle));
      }
      break;
    }
#elif defined(OS_MAC)
    case IPCZ_OS_HANDLE_MACH_SEND_RIGHT: {
      mach_port_t port = static_cast<mach_port_t>(os_handle.value);
      if (port != MACH_PORT_NULL) {
        return OSHandle(port, Type::kMachSendRight);
      }
      break;
    }

    case IPCZ_OS_HANDLE_MACH_RECEIVE_RIGHT: {
      mach_port_t port = static_cast<mach_port_t>(os_handle.value);
      if (port != MACH_PORT_NULL) {
        return OSHandle(port, Type::kMachReceiveRight);
      }
      break;
    }
#endif

#if defined(OS_POSIX) || defined(OS_FUCHSIA)
    case IPCZ_OS_HANDLE_FILE_DESCRIPTOR:
      return OSHandle(static_cast<int>(os_handle.value));
#endif
  }

  return OSHandle();
}

void OSHandle::reset() {
  Type type = Type::kInvalid;
  std::swap(type_, type);
  switch (type) {
    case Type::kInvalid:
      return;

#if defined(OS_WIN)
    case Type::kHandle:
      ::CloseHandle(handle_);
      return;
#endif

#if defined(OS_FUCHSIA)
    case Type::kHandle:
      handle_.reset();
      return;
#endif

#if defined(OS_MAC)
    case Type::kMachSendRight: {
      kern_return_t kr =
          mach_port_deallocate(mach_task_self(), mach_send_right_);
      ABSL_ASSERT(kr == KERN_SUCCESS);
      return;
    }

    case Type::kMachReceiveRight: {
      kern_return_t kr = mach_port_mod_refs(
          mach_task_self(), mach_receive_right_, MACH_PORT_RIGHT_RECEIVE, -1);
      ABSL_ASSERT(kr == KERN_SUCCESS);
      return;
    }
#endif

#if defined(OS_POSIX) || defined(OS_FUCHSIA)
    case Type::kFileDescriptor: {
      int rv = close(fd_);
      ABSL_ASSERT(rv == 0 || errno == EINTR);
      return;
    }
#endif
  }
}

void OSHandle::release() {
#if defined(OS_FUCHSIA)
  if (type_ == Type::kHandle) {
    (void)handle_.release();
  }
#endif
  type_ = Type::kInvalid;
}

OSHandle OSHandle::Clone() const {
  switch (type_) {
    case Type::kInvalid:
      return OSHandle();

#if defined(OS_WIN)
    case Type::kHandle: {
      ABSL_ASSERT(handle_ != INVALID_HANDLE_VALUE);

      HANDLE dupe;
      BOOL result = ::DuplicateHandle(::GetCurrentProcess(), handle_,
                                      ::GetCurrentProcess(), &dupe, 0, FALSE,
                                      DUPLICATE_SAME_ACCESS);
      if (!result) {
        return OSHandle();
      }

      ABSL_ASSERT(dupe != INVALID_HANDLE_VALUE);
      return OSHandle(dupe);
    }
#endif

#if defined(OS_FUCHSIA)
    case Type::kHandle:
      ABSL_ASSERT(handle_.is_valid());

      zx::handle dupe;
      zx_status_t result = handle_.duplicate(ZX_RIGHT_SAME_RIGHTS, &dupe);
      return OSHandle(std::move(dupe));
#endif

#if defined(OS_MAC)
    case Type::kMachSendRight: {
      ABSL_ASSERT(mach_send_right_ != MACH_PORT_NULL);
      kern_return_t kr = mach_port_mod_refs(mach_task_self(), mach_send_right_,
                                            MACH_PORT_RIGHT_SEND, 1);
      if (kr != KERN_SUCCESS) {
        return OSHandle();
      }
      return OSHandle(mach_send_right_, Type::kMachSendRight);
    }

    case Type::kMachReceiveRight: {
      // Not supported.
      ABSL_ASSERT(false);
      return OSHandle();
    }
#endif

#if defined(OS_POSIX) || defined(OS_FUCHSIA)
    case Type::kFileDescriptor: {
      ABSL_ASSERT(fd_ != -1);
      int dupe = dup(fd_);
      // ABSL_ASSERT(dupe >= 0);
      return OSHandle(dupe);
    }
#endif
  }
}

}  // namespace ipcz
