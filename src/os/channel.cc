// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "os/channel.h"

#include <stdio.h>

#include <algorithm>
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

Handle Channel::TakeHandle() {
  StopListening();
  return std::move(handle_);
}

void Channel::Listen(MessageHandler handler) {
  StopListening();

  if (read_buffer_.empty()) {
    read_buffer_.resize(kMaxDataSize);
    unread_data_ = absl::Span<uint8_t>(read_buffer_.data(), 0);
    handle_buffer_.resize(4);
    unread_handles_ = absl::Span<os::Handle>(handle_buffer_.data(), 0);
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

void Channel::Send(Message message) {
  {
    absl::MutexLock lock(&queue_mutex_);
    if (!outgoing_queue_.empty()) {
      outgoing_queue_.emplace_back(message);
      return;
    }
  }

  absl::optional<Message> m = SendInternal(message);
  if (m) {
    absl::MutexLock lock(&queue_mutex_);
    outgoing_queue_.emplace_back(*m);
  }
}

absl::optional<Channel::Message> Channel::SendInternal(Message message) {
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
  absl::optional<Message> remainder;
  for (;;) {
    ssize_t result;
    {
      absl::MutexLock lock(&send_mutex_);
      result = sendmsg(handle_.fd(), &msg, MSG_NOSIGNAL);
    }
    if (result < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        remainder = message;
      } else if (errno == EINTR) {
        continue;
      } else {
        // Unrecoverable.
        LOG(ERROR) << "broken Channel";
        return absl::nullopt;
      }
    } else if (result < static_cast<ssize_t>(message.data.size())) {
      // partial send - handles are fine, but may need to send more data later.
      remainder = Message(Data(message.data.subspan(result)));
    }

    return remainder;
  }

#endif
}

void Channel::ReadMessagesOnIOThread(MessageHandler handler,
                                     Event shutdown_event) {
  if (!handle_.is_valid() || !shutdown_event.is_valid()) {
    return;
  }

  for (;;) {
    bool have_out_messages = false;
    {
      absl::MutexLock lock(&queue_mutex_);
      have_out_messages = !outgoing_queue_.empty();
    }
#if defined(OS_POSIX)
    pollfd poll_fds[2];
    poll_fds[0].fd = handle_.fd();
    poll_fds[0].events = POLLIN | (have_out_messages ? POLLOUT : 0);
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

    if (poll_fds[0].revents & POLLOUT) {
      TryFlushingQueue();
    }

    if ((poll_fds[0].revents & POLLIN) == 0) {
      continue;
    }

    const size_t unread_offset = unread_data_.data() - read_buffer_.data();
    size_t capacity = read_buffer_.size() - unread_data_.size() - unread_offset;
    if (capacity < kMaxDataSize) {
      size_t new_size = read_buffer_.size() * 2;
      read_buffer_.resize(new_size);
      unread_data_ = absl::Span<uint8_t>(read_buffer_.data() + unread_offset,
                                         unread_data_.size());
      capacity = read_buffer_.size() - unread_data_.size() - unread_offset;
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
          const size_t unread_handles_offset =
              unread_handles_.data() - handle_buffer_.data();
          if (unread_handles_offset + unread_handles_.size() + num_fds >
              handle_buffer_.size()) {
            handle_buffer_.resize(std::max(handle_buffer_.size() * 2,
                                           handle_buffer_.size() + num_fds));
          }
          absl::Span<os::Handle> new_handles(
              handle_buffer_.data() + unread_handles_.size(), num_fds);
          unread_handles_ = {handle_buffer_.data() + unread_handles_offset,
                             unread_handles_.size() + num_fds};
          for (size_t i = 0; i < num_fds; ++i) {
            new_handles[i] = os::Handle(fds[i]);
          }
        }
      }
      ABSL_ASSERT((msg.msg_flags & MSG_CTRUNC) == 0);
    }

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
#endif
  }
}

void Channel::TryFlushingQueue() {
  size_t i = 0;
  for (;; ++i) {
    absl::optional<Message> m;
    {
      absl::MutexLock lock(&queue_mutex_);
      if (i >= outgoing_queue_.size()) {
        break;
      }
      m = outgoing_queue_[i].AsMessage();
    }

    m = SendInternal(*m);
    if (m) {
      // still at least partially blocked
      absl::MutexLock lock(&queue_mutex_);
      outgoing_queue_[i] = DeferredMessage(*m);
      break;
    }
  }

  absl::MutexLock lock(&queue_mutex_);
  if (i == 0) {
    // no real progress
    return;
  }
  if (i == outgoing_queue_.size()) {
    // finished!
    outgoing_queue_.clear();
    return;
  }

  // partial progress
  std::move(outgoing_queue_.begin() + i, outgoing_queue_.end(),
            outgoing_queue_.begin());
  outgoing_queue_.resize(outgoing_queue_.size() - i);
}

Channel::DeferredMessage::DeferredMessage() = default;

Channel::DeferredMessage::DeferredMessage(Message& m) {
  data = std::vector<uint8_t>(m.data.begin(), m.data.end());
  handles.resize(m.handles.size());
  std::move(m.handles.begin(), m.handles.end(), handles.begin());
}

Channel::DeferredMessage::DeferredMessage(DeferredMessage&&) = default;

Channel::DeferredMessage& Channel::DeferredMessage::operator=(
    DeferredMessage&&) = default;

Channel::DeferredMessage::~DeferredMessage() = default;

Channel::Message Channel::DeferredMessage::AsMessage() {
  return Message(Data(absl::MakeSpan(data)), absl::MakeSpan(handles));
}

}  // namespace os
}  // namespace ipcz
