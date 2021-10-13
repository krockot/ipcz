// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "os/channel.h"

#include <stdio.h>

#include <vector>

#include "build/build_config.h"

#if defined(OS_POSIX)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

namespace ipcz {
namespace os {

Channel::Data::Data() = default;

Channel::Data::Data(absl::Span<const uint8_t> data) : Span(data) {}

Channel::Data::Data(absl::Span<const char> str)
    : Span(reinterpret_cast<const uint8_t*>(str.data()), str.size()) {}

absl::Span<const char> Channel::Data::AsString() const {
  return absl::MakeSpan(reinterpret_cast<const char*>(data()), size());
}

Channel::Message::Message(Data data) : data(data) {}

Channel::Message::Message(Data data, absl::Span<Handle> handles)
    : data(data), handles(handles) {}

Channel::Message::Message(const Message&) = default;

Channel::Message& Channel::Message::operator=(const Message&) = default;

Channel::Message::~Message() = default;

Channel::Channel() = default;

Channel::Channel(Handle handle) : handle_(std::move(handle)) {}

Channel::Channel(Channel&& other) {
  other.StopListening();
  handle_ = std::move(other.handle_);
}

Channel& Channel::operator=(Channel&& other) {
  StopListening();
  other.StopListening();
  handle_ = std::move(other.handle_);
  return *this;
}

Channel::~Channel() {
  StopListening();
}

// static
std::pair<Channel, Channel> Channel::CreateChannelPair() {
#if defined(OS_POSIX)
  int fds[2];
  int result = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
  if (result != 0) {
    return {};
  }

  bool ok = fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0;
  ok = ok && (fcntl(fds[1], F_SETFL, O_NONBLOCK) == 0);
  if (!ok) {
    close(fds[0]);
    close(fds[1]);
    return {};
  }

  return std::make_pair(Channel(Handle(fds[0])), Channel(Handle(fds[1])));
#endif
}

// static
Channel Channel::FromIpczOSTransport(const IpczOSTransport& transport) {
#if defined(OS_WIN)
  if (transport.type != IPCZ_OS_TRANSPORT_WINDOWS_IO) {
    return Channel();
  }
#elif defined(OS_FUCHSIA)
  if (transport.type != IPCZ_OS_TRANSPORT_FUCHSIA_CHANNEL) {
    return Channel();
  }
#elif defined(OS_POSIX)
  if (transport.type != IPCZ_OS_TRANSPORT_UNIX_SOCKET) {
    return Channel();
  }
#endif

  if (transport.num_handles != 1 || !transport.handles) {
    return Channel();
  }

  return Channel(Handle::FromIpczOSHandle(transport.handles[0]));
}

Handle Channel::TakeHandle() {
  StopListening();
  return std::move(handle_);
}

void Channel::Listen(MessageHandler handler) {
  StopListening();

  Event shutdown_event;
  shutdown_notifier_ = shutdown_event.MakeNotifier();
  io_thread_.emplace(&Channel::ReadMessagesOnIOThread, this, handler,
                     std::move(shutdown_event));
}

void Channel::StopListening() {
  if (io_thread_) {
    ABSL_ASSERT(shutdown_notifier_.is_valid());
    shutdown_notifier_.Notify();
    io_thread_->join();
    io_thread_.reset();
  }
}

void Channel::Reset() {
  StopListening();
  handle_.reset();
}

bool Channel::Send(Message message) {
  ABSL_ASSERT(handle_.is_valid());

#if defined(OS_POSIX)
  if (message.handles.empty()) {
    return send(handle_.fd(), message.data.data(), message.data.size(),
                MSG_NOSIGNAL) == static_cast<ssize_t>(message.data.size());
  }

  iovec iov = {const_cast<uint8_t*>(message.data.data()), message.data.size()};

  ABSL_ASSERT(message.handles.size() <= 128);
  char cmsg_buf[CMSG_SPACE(128 * sizeof(int))];
  struct msghdr msg = {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsg_buf;
  msg.msg_controllen = CMSG_LEN(message.handles.size() * sizeof(int));
  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(message.handles.size() * sizeof(int));
  for (size_t i = 0; i < message.handles.size(); ++i) {
    reinterpret_cast<int*>(CMSG_DATA(cmsg))[i] = message.handles[i].fd();
  }
  for (;;) {
    ssize_t result = sendmsg(handle_.fd(), &msg, MSG_NOSIGNAL);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }

    if (result < static_cast<ssize_t>(message.data.size())) {
      return false;
    }

    return true;
  }

#endif
}

void Channel::ReadMessagesOnIOThread(MessageHandler handler,
                                     Event shutdown_event) {
  if (!handle_.is_valid() || !shutdown_event.is_valid()) {
    return;
  }

#if defined(OS_POSIX)
  for (;;) {
    pollfd poll_fds[2];
    poll_fds[0].fd = handle_.fd();
    poll_fds[0].events = POLLIN;
    poll_fds[1].fd = shutdown_event.handle().fd();
    poll_fds[1].events = POLLIN;
    int poll_result;
    do {
      poll_result = poll(poll_fds, 2, -1);
    } while (poll_result == -1 && (errno == EINTR || errno == EAGAIN));
    ABSL_ASSERT(poll_result > 0);

    if (poll_fds[0].revents & POLLERR) {
      return;
    }

    if (poll_fds[1].revents & POLLIN) {
      shutdown_event.Wait();
      return;
    }

    ABSL_ASSERT(poll_fds[0].revents & POLLIN);

    uint8_t data[4096];
    struct iovec iov = {data, 4096};
    char cmsg_buf[CMSG_SPACE(128 * sizeof(int))];
    struct msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);
    ssize_t result;
    do {
      result = recvmsg(handle_.fd(), &msg, 0);
    } while (result == -1 && errno == EINTR);
    if (result <= 0) {
      return;
    }

    if (msg.msg_controllen == 0) {
      handler(Data(absl::MakeSpan(&data[0], result)));
      continue;
    }

    ABSL_ASSERT((msg.msg_flags & MSG_CTRUNC) == 0);

    std::vector<Handle> handles;
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        size_t payload_length = cmsg->cmsg_len - CMSG_LEN(0);
        ABSL_ASSERT(payload_length % sizeof(int) == 0);
        size_t num_fds = payload_length / sizeof(int);
        const int* fds = reinterpret_cast<int*>(CMSG_DATA(cmsg));
        for (size_t i = 0; i < num_fds; ++i) {
          handles.emplace_back(fds[i]);
        }
      }
    }

    handler(Message(Data(absl::MakeSpan(&data[0], result)),
                    absl::MakeSpan(handles)));
  }
#endif
}

}  // namespace os
}  // namespace ipcz
