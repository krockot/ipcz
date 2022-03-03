// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "reference_drivers/os_handle.h"

#include <algorithm>

#include "build/build_config.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#elif BUILDFLAG(IS_FUCHSIA)
#include <lib/fdio/limits.h>
#include <lib/zx/handle.h>
#include <zircon/status.h>
#elif BUILDFLAG(IS_MAC)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
#include <errno.h>
#include <unistd.h>
#endif

namespace ipcz::reference_drivers {

OSHandle::OSHandle() = default;

#if BUILDFLAG(IS_WIN)
OSHandle::OSHandle(HANDLE handle) : type_(Type::kHandle), handle_(handle) {}
#elif BUILDFLAG(IS_FUCHSIA)
OSHandle::OSHandle(zx::handle handle)
    : type_(Type::kHandle), handle_(std::move(handle)) {}
#elif BUILDFLAG(IS_MAC)
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

#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
OSHandle::OSHandle(int fd) : type_(Type::kFileDescriptor), fd_(fd) {}
#endif

OSHandle::OSHandle(OSHandle&& other) : type_(other.type_) {
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_FUCHSIA)
  handle_ = std::move(other.handle_);
#elif BUILDFLAG(IS_MAC)
  mach_send_right_ = other.mach_send_right_;
  mach_receive_right_ = other.mach_receive_right_;
#endif

#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
  fd_ = other.fd_;
#endif

  other.type_ = Type::kInvalid;
}

OSHandle& OSHandle::operator=(OSHandle&& other) {
  reset();
  type_ = other.type_;

#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_FUCHSIA)
  handle_ = std::move(other.handle_);
#elif BUILDFLAG(IS_MAC)
  mach_send_right_ = other.mach_send_right_;
  mach_receive_right_ = other.mach_receive_right_;
#endif

#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
  fd_ = other.fd_;
#endif

  other.type_ = Type::kInvalid;
  return *this;
}

OSHandle::~OSHandle() {
  reset();
}

void OSHandle::reset() {
  Type type = Type::kInvalid;
  std::swap(type_, type);
  switch (type) {
    case Type::kInvalid:
      return;

#if BUILDFLAG(IS_WIN)
    case Type::kHandle:
      ::CloseHandle(handle_);
      return;
#endif

#if BUILDFLAG(IS_FUCHSIA)
    case Type::kHandle:
      handle_.reset();
      return;
#endif

#if BUILDFLAG(IS_MAC)
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

#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
    case Type::kFileDescriptor: {
      int rv = close(fd_);
      ABSL_ASSERT(rv == 0 || errno == EINTR);
      return;
    }
#endif
  }
}

void OSHandle::release() {
#if BUILDFLAG(IS_FUCHSIA)
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

#if BUILDFLAG(IS_WIN)
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

#if BUILDFLAG(IS_FUCHSIA)
    case Type::kHandle:
      ABSL_ASSERT(handle_.is_valid());

      zx::handle dupe;
      zx_status_t result = handle_.duplicate(ZX_RIGHT_SAME_RIGHTS, &dupe);
      return OSHandle(std::move(dupe));
#endif

#if BUILDFLAG(IS_MAC)
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

#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
    case Type::kFileDescriptor: {
      ABSL_ASSERT(fd_ != -1);
      int dupe = dup(fd_);
      // ABSL_ASSERT(dupe >= 0);
      return OSHandle(dupe);
    }
#endif
  }
}

}  // namespace ipcz::reference_drivers
