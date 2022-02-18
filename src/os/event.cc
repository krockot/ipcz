// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "os/event.h"

#include "third_party/abseil-cpp/absl/base/macros.h"

#if defined(OS_LINUX)
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

#if defined(OS_WIN)
#include <windows.h>
#endif

namespace ipcz {
namespace os {

Event::Notifier::Notifier() = default;

Event::Notifier::Notifier(Handle handle) : handle_(std::move(handle)) {}

Event::Notifier::Notifier(Notifier&&) = default;

Event::Notifier& Event::Notifier::operator=(Notifier&&) = default;

Event::Notifier::~Notifier() = default;

Handle Event::Notifier::TakeHandle() {
  return std::move(handle_);
}

void Event::Notifier::Notify() {
  ABSL_ASSERT(is_valid());

#if defined(OS_LINUX)
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
#elif defined(OS_WIN)
  ::SetEvent(handle_.handle());
#else
#error "Missing Event impl for this platform.";
#endif
}

Event::Notifier Event::Notifier::Clone() {
  return Notifier(handle_.Clone());
}

Event::Event() {
#if defined(OS_LINUX)
  int fd = eventfd(0, EFD_NONBLOCK);
  ABSL_ASSERT(fd >= 0);
  handle_ = Handle(fd);
#elif defined(OS_WIN)
  HANDLE h = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
  ABSL_ASSERT(h != INVALID_HANDLE_VALUE);
  handle_ = Handle(h);
#else
#error "Missing Event impl for this platform.";
#endif
}

Event::Event(Handle handle) : handle_(std::move(handle)) {}

Event::Event(Event&&) = default;

Event& Event::operator=(Event&&) = default;

Event::~Event() = default;

Handle Event::TakeHandle() {
  return std::move(handle_);
}

Event::Notifier Event::MakeNotifier() {
  ABSL_ASSERT(is_valid());

#if defined(OS_LINUX) || defined(OS_WIN)
  return Notifier(handle_.Clone());
#else
#error "Missing Event impl for this platform.";
#endif
}

void Event::Wait() {
  ABSL_ASSERT(is_valid());

#if defined(OS_LINUX)
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
#elif defined(OS_WIN)
  ::WaitForSingleObject(handle_.handle(), INFINITE);
  ::ResetEvent(handle_.handle());
#else
#error "Missing Event impl for this platform.";
#endif
}

}  // namespace os
}  // namespace ipcz
