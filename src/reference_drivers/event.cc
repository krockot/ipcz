// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "reference_drivers/event.h"

#include "third_party/abseil-cpp/absl/base/macros.h"

#if BUILDFLAG(IS_LINUX)
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

namespace ipcz {
namespace reference_drivers {

Event::Notifier::Notifier() = default;

Event::Notifier::Notifier(OSHandle handle) : handle_(std::move(handle)) {}

Event::Notifier::Notifier(Notifier&&) = default;

Event::Notifier& Event::Notifier::operator=(Notifier&&) = default;

Event::Notifier::~Notifier() = default;

OSHandle Event::Notifier::TakeHandle() {
  return std::move(handle_);
}

void Event::Notifier::Notify() {
  ABSL_ASSERT(is_valid());

#if BUILDFLAG(IS_LINUX)
  const uint64_t value = 0xfffffffffffffffe;
  int result;
  do {
    result = write(handle_.fd(), &value, sizeof(value));
  } while (result == -1 && errno == EINTR);

  if (result > 0) {
    ABSL_ASSERT(result = 8);
    return;
  }

  if (result == -1) {
    ABSL_ASSERT(errno == EAGAIN);
    return;
  }
#elif BUILDFLAG(IS_WIN)
  ::SetEvent(handle_.handle());
#else
#error "Missing Event impl for this platform.";
#endif
}

Event::Notifier Event::Notifier::Clone() {
  return Notifier(handle_.Clone());
}

Event::Event() {
#if BUILDFLAG(IS_LINUX)
  int fd = eventfd(0, EFD_NONBLOCK);
  ABSL_ASSERT(fd >= 0);
  handle_ = OSHandle(fd);
#elif BUILDFLAG(IS_WIN)
  HANDLE h = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
  ABSL_ASSERT(h != INVALID_HANDLE_VALUE);
  handle_ = OSHandle(h);
#else
#error "Missing Event impl for this platform.";
#endif
}

Event::Event(OSHandle handle) : handle_(std::move(handle)) {}

Event::Event(Event&&) = default;

Event& Event::operator=(Event&&) = default;

Event::~Event() = default;

OSHandle Event::TakeHandle() {
  return std::move(handle_);
}

Event::Notifier Event::MakeNotifier() {
  ABSL_ASSERT(is_valid());

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN)
  return Notifier(handle_.Clone());
#else
#error "Missing Event impl for this platform.";
#endif
}

void Event::Wait() {
  ABSL_ASSERT(is_valid());

#if BUILDFLAG(IS_LINUX)
  pollfd fd;
  fd.fd = handle_.fd();
  fd.events = POLLIN;
  int result;
  do {
    result = poll(&fd, 1, -1);
  } while (result == -1 && (errno == EAGAIN || errno == EINTR));

  uint64_t value;
  do {
    result = read(handle_.fd(), &value, sizeof(value));
  } while (result == -1 && errno == EINTR);

  ABSL_ASSERT(result == 8 || (result == -1 && errno == EAGAIN));
#elif BUILDFLAG(IS_WIN)
  ::WaitForSingleObject(handle_.handle(), INFINITE);
  ::ResetEvent(handle_.handle());
#else
#error "Missing Event impl for this platform.";
#endif
}

}  // namespace reference_drivers
}  // namespace ipcz
