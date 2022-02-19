// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "os/channel.h"

#include <stdio.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "build/build_config.h"
#include "debug/hex_dump.h"
#include "debug/log.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "util/random.h"

#if defined(OS_POSIX)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

#if defined(OS_WIN)
#include <windows.h>
#endif

namespace ipcz {
namespace os {

namespace {

// "4 kB should be enough for anyone." -- anonymous
constexpr size_t kMaxDataSize = 4096;

#if defined(OS_POSIX)
constexpr size_t kMaxHandlesPerMessage = 64;
#endif

}  // namespace

#if defined(OS_WIN)
struct Channel::PendingIO {
  PendingIO() = default;
  ~PendingIO() = default;

  static void CompletionRoutine(DWORD error,
                                DWORD num_bytes_transferred,
                                LPOVERLAPPED overlapped) {
    auto* io = reinterpret_cast<PendingIO*>(overlapped);
    if (error != 0) {
      io->OnComplete(false, 0);
      return;
    }

    io->OnComplete(true, static_cast<size_t>(num_bytes_transferred));
  }

  void Write(HANDLE handle, absl::Span<const uint8_t> data) {
    ABSL_ASSERT(!buffer_);
    buffer_ = std::make_unique<uint8_t[]>(data.size() + 16);

    uint32_t* header = reinterpret_cast<uint32_t*>(buffer_.get());
    header[0] = static_cast<uint32_t>(data.size() + 16);
    header[1] = 0;
    header[2] = 0;
    header[3] = 0;
    memcpy(header + 4, data.data(), data.size());

    io_callback_ = [this](bool, size_t) { delete this; };
    BOOL result = ::WriteFileEx(handle, buffer_.get(),
                                static_cast<DWORD>(data.size() + 16), &io,
                                &CompletionRoutine);
    ABSL_ASSERT(result);
  }

  using IOCallback = std::function<void(bool success, size_t num_bytes)>;
  void Read(HANDLE handle, absl::Span<uint8_t> storage, IOCallback callback) {
    if (!buffer_) {
      buffer_ = std::make_unique<uint8_t[]>(kMaxDataSize);
    }
    io_callback_ = callback;
    BOOL result = ::ReadFileEx(handle, storage.data(), storage.size(), &io,
                               &CompletionRoutine);
    ABSL_ASSERT(result);
  }

 private:
  void OnComplete(bool success, size_t num_bytes_transferred) {
    io_callback_(success, num_bytes_transferred);
  }

  OVERLAPPED io = {0};
  std::unique_ptr<uint8_t[]> buffer_;
  IOCallback io_callback_;
};
#endif

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
#elif defined(OS_WIN)
  std::wstringstream ss;
  ss << "\\\\.\\pipe\\ipcz." << ::GetCurrentProcessId() << "."
     << ::GetCurrentThreadId() << "." << RandomUint64();
  std::wstring pipe_name = ss.str();
  DWORD kOpenMode =
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE;
  const DWORD kPipeMode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE;
  HANDLE handle0 = ::CreateNamedPipeW(pipe_name.c_str(), kOpenMode, kPipeMode,
                                      1, 4096, 4096, 5000, nullptr);
  ABSL_ASSERT(handle0 != INVALID_HANDLE_VALUE);

  const DWORD kDesiredAccess = GENERIC_READ | GENERIC_WRITE;
  DWORD kFlags =
      SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS | FILE_FLAG_OVERLAPPED;
  SECURITY_ATTRIBUTES security_attributes = {sizeof(SECURITY_ATTRIBUTES),
                                             nullptr, TRUE};
  HANDLE handle1 =
      ::CreateFileW(pipe_name.c_str(), kDesiredAccess, 0, &security_attributes,
                    OPEN_EXISTING, kFlags, nullptr);
  ABSL_ASSERT(handle1 != INVALID_HANDLE_VALUE);

  return std::make_pair(Channel(Handle(handle0)), Channel(Handle(handle1)));
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
  Event outgoing_queue_event;
  shutdown_notifier_ = shutdown_event.MakeNotifier();
  outgoing_queue_notifier_ = outgoing_queue_event.MakeNotifier();
  io_thread_.emplace(&Channel::ReadMessagesOnIOThread, this, handler,
                     std::move(shutdown_event),
                     std::move(outgoing_queue_event));
}

void Channel::StopListening() {
  if (io_thread_) {
#if defined(OS_WIN)
    if (handle_.is_valid()) {
      ::CancelIo(handle_.handle());
    }
#endif
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

  bool nonempty_queue_was_empty = false;
  absl::optional<Message> m = SendInternal(message);
  if (m) {
    absl::MutexLock lock(&queue_mutex_);
    nonempty_queue_was_empty = outgoing_queue_.empty();
    outgoing_queue_.emplace_back(*m);
  }

  if (nonempty_queue_was_empty) {
    outgoing_queue_notifier_.Notify();
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

  uint32_t header[4];
  header[0] = static_cast<uint32_t>(message.data.size() + 16);
  header[1] = static_cast<uint32_t>(message.handles.size());
  header[2] = 0;
  header[3] = 0;
  iovec iovs[] = {
      {reinterpret_cast<uint8_t*>(&header[0]), 16},
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
#elif defined(OS_WIN)
  // Windows does not transport handles out-of-band from the rest of the data.
  ABSL_ASSERT(message.handles.empty());

  auto* io = new PendingIO();
  io->Write(handle_.handle(), message.data);
  return absl::nullopt;
#endif
}

void Channel::ReadMessagesOnIOThread(MessageHandler handler,
                                     Event shutdown_event,
                                     Event outgoing_queue_event) {
  if (!handle_.is_valid() || !shutdown_event.is_valid() ||
      !outgoing_queue_event.is_valid()) {
    return;
  }

#if defined(OS_WIN)
  StartRead();
#endif

  for (;;) {
    bool have_out_messages = false;
    {
      absl::MutexLock lock(&queue_mutex_);
      have_out_messages = !outgoing_queue_.empty();
    }
#if defined(OS_POSIX)
    pollfd poll_fds[3];
    poll_fds[0].fd = handle_.fd();
    poll_fds[0].events = POLLIN | (have_out_messages ? POLLOUT : 0);
    poll_fds[1].fd = shutdown_event.handle().fd();
    poll_fds[1].events = POLLIN;
    poll_fds[2].fd = outgoing_queue_event.handle().fd();
    poll_fds[2].events = POLLIN;
    int poll_result;
    do {
      poll_result = poll(poll_fds, 3, -1);
    } while (poll_result == -1 && (errno == EINTR || errno == EAGAIN));
    ABSL_ASSERT(poll_result > 0);

    if (poll_fds[0].revents & POLLERR) {
      return;
    }

    if (poll_fds[1].revents & POLLIN) {
      shutdown_event.Wait();
      return;
    }

    if (poll_fds[2].revents & POLLIN) {
      outgoing_queue_event.Wait();
      continue;
    }

    if (poll_fds[0].revents & POLLOUT) {
      TryFlushingQueue();
    }

    if ((poll_fds[0].revents & POLLIN) == 0) {
      continue;
    }

    absl::Span<uint8_t> storage = EnsureReadCapacity();
    struct iovec iov = {storage.data(), storage.size()};
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

    CommitRead(static_cast<size_t>(result));

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
#elif defined(OS_WIN)
    // We don't queue outgoing messages on Windows.
    ABSL_ASSERT(!have_out_messages);

    switch (::WaitForSingleObjectEx(shutdown_event.handle().handle(), INFINITE,
                                    TRUE)) {
      case WAIT_OBJECT_0:
        shutdown_event.Wait();
        return;

      case WAIT_IO_COMPLETION:
        break;

      default:
        io_error_ = true;
        break;
    }

    if (io_error_) {
      LOG(ERROR) << "Disconnecting Channel for I/O error";
      return;
    }
#endif

    while (unread_data_.size() >= 16) {
      uint32_t* header_data = reinterpret_cast<uint32_t*>(unread_data_.data());
      if (unread_data_.size() < header_data[0] ||
          unread_handles_.size() < header_data[1]) {
        break;
      }

      auto data_view =
          absl::MakeSpan(unread_data_.data() + 16, header_data[0] - 16);
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

absl::Span<uint8_t> Channel::EnsureReadCapacity() {
  const size_t unread_offset = unread_data_.data() - read_buffer_.data();
  size_t capacity = read_buffer_.size() - unread_data_.size() - unread_offset;
  if (capacity < kMaxDataSize) {
    size_t new_size = read_buffer_.size() * 2;
    read_buffer_.resize(new_size);
    unread_data_ = absl::Span<uint8_t>(read_buffer_.data() + unread_offset,
                                       unread_data_.size());
    capacity = read_buffer_.size() - unread_data_.size() - unread_offset;
  }
  return {unread_data_.end(), capacity};
}

void Channel::CommitRead(size_t num_bytes) {
  unread_data_ =
      absl::Span<uint8_t>(unread_data_.data(), unread_data_.size() + num_bytes);
}

#if defined(OS_WIN)
void Channel::StartRead() {
  if (!pending_read_) {
    pending_read_ = std::make_unique<PendingIO>();
  }

  absl::Span<uint8_t> storage = EnsureReadCapacity();
  pending_read_->Read(
      handle_.handle(), storage,
      [channel = this](bool success, size_t num_bytes_transferred) {
        if (!success) {
          channel->io_error_ = true;
          return;
        }

        channel->CommitRead(num_bytes_transferred);
        channel->StartRead();
      });
}
#endif

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
