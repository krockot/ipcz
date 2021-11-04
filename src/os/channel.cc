// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "os/channel.h"

#include <stdio.h>

#include <vector>

#include "build/build_config.h"
#include "debug/hex_dump.h"
#include "debug/log.h"

#if defined(OS_POSIX)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

namespace ipcz {
namespace os {

namespace {

// "4 kB should be enough for anyone." -- anonymous
constexpr size_t kMaxDataSize = 4096;
constexpr size_t kMaxHandlesPerMessage = 64;

}  // namespace

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

// static
bool Channel::ToOSTransport(Channel channel, OSTransportWithHandle& out) {
#if defined(OS_WIN)
  out.type = IPCZ_OS_TRANSPORT_WINDOWS_IO;
#elif defined(OS_FUCHSIA)
  out.type = IPCZ_OS_TRANSPORT_FUCHSIA_CHANNEL;
#elif defined(OS_POSIX)
  out.type = IPCZ_OS_TRANSPORT_UNIX_SOCKET;
#endif
  return os::Handle::ToIpczOSHandle(channel.TakeHandle(), &out.handle);
}

Handle Channel::TakeHandle() {
  StopListening();
  return std::move(handle_);
}

void Channel::Listen(MessageHandler handler) {
  StopListening();

  if (read_buffer_.empty()) {
    read_buffer_.resize(kMaxDataSize);
    unread_data_ = absl::Span<uint8_t>(read_buffer_.data(), 0);
  }

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
  size_t num_valid_handles = 0;
  for (const Handle& handle : message.handles) {
    if (handle.is_valid()) {
      ++num_valid_handles;
    }
  }

  uint32_t header[2];
  header[0] = static_cast<uint32_t>(message.data.size() + 8);
  header[1] = static_cast<uint32_t>(message.handles.size());
  iovec iovs[] = {
      {reinterpret_cast<uint8_t*>(&header[0]), 8},
      {const_cast<uint8_t*>(message.data.data()), message.data.size()},
  };

  ABSL_ASSERT(num_valid_handles <= kMaxHandlesPerMessage);
  char cmsg_buf[CMSG_SPACE(kMaxHandlesPerMessage * sizeof(int))];
  struct msghdr msg = {};
  msg.msg_iov = &iovs[0];
  msg.msg_iovlen = 2;
  msg.msg_control = cmsg_buf;
  msg.msg_controllen = CMSG_LEN(num_valid_handles * sizeof(int));
  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(num_valid_handles * sizeof(int));
  size_t next_handle = 0;
  for (const Handle& handle : message.handles) {
    if (handle.is_valid()) {
      reinterpret_cast<int*>(CMSG_DATA(cmsg))[next_handle++] = handle.fd();
    }
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

    size_t capacity = read_buffer_.size() - unread_data_.size();
    if (capacity < kMaxDataSize) {
      size_t new_size = read_buffer_.size() * 2;
      size_t unread_offset = unread_data_.data() - read_buffer_.data();
      read_buffer_.resize(new_size);
      unread_data_ = absl::Span<uint8_t>(read_buffer_.data() + unread_offset,
                                         unread_data_.size());
      capacity = read_buffer_.size() - unread_data_.size();
    }
    uint8_t* data = unread_data_.end();
    struct iovec iov = {data, capacity};
    char cmsg_buf[CMSG_SPACE(kMaxHandlesPerMessage * sizeof(int))];
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

    unread_data_ =
        absl::Span<uint8_t>(unread_data_.data(), unread_data_.size() + result);

    if (msg.msg_controllen > 0) {
      for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg;
           cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
          size_t payload_length = cmsg->cmsg_len - CMSG_LEN(0);
          ABSL_ASSERT(payload_length % sizeof(int) == 0);
          size_t num_fds = payload_length / sizeof(int);
          const int* fds = reinterpret_cast<int*>(CMSG_DATA(cmsg));
          size_t unread_handle_offset =
              (unread_handles_.data() - handle_buffer_.data()) /
              sizeof(os::Handle);
          for (size_t i = 0; i < num_fds; ++i) {
            handle_buffer_.emplace_back(fds[i]);
          }
          unread_handles_ = absl::Span<os::Handle>(
              handle_buffer_.data() + unread_handle_offset,
              unread_handles_.size() + num_fds);
        }
      }
      ABSL_ASSERT((msg.msg_flags & MSG_CTRUNC) == 0);
    }

    // TODO: this is a mega hack - decide if we want to frame here or in
    // NodeLink, will determine message structure layering
    while (unread_data_.size() >= 8) {
      uint32_t* header_data = reinterpret_cast<uint32_t*>(unread_data_.data());
      if (unread_data_.size() < header_data[0] ||
          unread_handles_.size() < header_data[1]) {
        break;
      }

      auto data_view =
          absl::MakeSpan(unread_data_.data() + 8, header_data[0] - 8);
      auto handle_view = absl::MakeSpan(unread_handles_.data(), header_data[1]);
      unread_data_ = unread_data_.subspan(header_data[0]);
      unread_handles_ = unread_handles_.subspan(header_data[1]);
      if (!handler(Message(Data(data_view), handle_view))) {
        LOG(ERROR) << "disconnecting Channel for bad message: "
                   << debug::HexDump(data_view);
        return;
      }

      if (unread_data_.empty()) {
        unread_data_ = absl::MakeSpan(read_buffer_.data(), 0);
      }
      if (unread_handles_.empty()) {
        unread_handles_ = absl::MakeSpan(handle_buffer_.data(), 0);
      }
    }
  }
#endif
}

}  // namespace os
}  // namespace ipcz
