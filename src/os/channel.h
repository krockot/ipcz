// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_OS_CHANNEL_H_
#define IPCZ_SRC_OS_CHANNEL_H_

#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "ipcz/ipcz.h"
#include "os/event.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/synchronization/notification.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace os {

// Generic OS communication channel abstraction. This may wrap a UNIX domain
// socket, a Windows I/O object (such as a named pipe), a Fuchsia channel, or
// a Mach port pair.
class Channel {
 public:
  class Data : public absl::Span<const uint8_t> {
   public:
    Data();
    Data(absl::Span<const uint8_t> data);
    Data(absl::Span<const char> str);

    template <size_t N>
    Data(const char str[N]) : Data(absl::MakeSpan(str)) {}

    absl::Span<const char> AsString() const;
  };

  struct Message {
    Message(Data data);
    Message(Data data, absl::Span<Handle> handles);
    Message(const Message&);
    Message& operator=(const Message&);
    ~Message();

    Data data;
    absl::Span<Handle> handles;
  };

  struct OSTransportWithHandle : IpczOSTransport {
    OSTransportWithHandle()
        : IpczOSTransport{sizeof(OSTransportWithHandle), 0, &handle, 1} {}

    IpczOSHandle handle = {sizeof(handle)};
  };

  // Creates a new Channel using `handle` as the endpoint to manipulate with
  // platform-specific I/O operations.
  //
  // If `handle` is a POSIX file descriptor, it should name a UNIX domain socket
  // implementing sendmsg() and send()/write().
  //
  // If `handle` is a Windows HANDLE, it should name an object that supports
  // WriteFile() and ReadFile() and overlapped I/O.
  //
  // If `handle` is a Fuchsia handle, it should name a zx_channel object.
  Channel();
  explicit Channel(Handle handle);
  Channel(Channel&&);
  Channel& operator=(Channel&&);
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  ~Channel();

  static std::pair<Channel, Channel> CreateChannelPair();

  bool is_valid() const { return handle_.is_valid(); }

  const Handle& handle() const { return handle_; }

  static Channel FromIpczOSTransport(const IpczOSTransport& transport);
  static bool ToOSTransport(Channel channel, OSTransportWithHandle& out);

  Handle TakeHandle();

  using MessageHandler = std::function<bool(Message)>;
  void Listen(MessageHandler handler);

  void StopListening();

  void Reset();

  void Send(Message message);

 private:
  // Attempts to send, without queueing, and if it fails to send any or all of
  // the message contents, returns a view of what's left.
  absl::optional<Message> SendInternal(Message message);
  void ReadMessagesOnIOThread(MessageHandler handler, Event shutdown_event);
  void TryFlushingQueue();

  Handle handle_;
  Event::Notifier shutdown_notifier_;
  absl::optional<std::thread> io_thread_;
  std::vector<uint8_t> read_buffer_;
  absl::Span<uint8_t> unread_data_;
  std::vector<os::Handle> handle_buffer_;
  absl::Span<os::Handle> unread_handles_;

  struct DeferredMessage {
    DeferredMessage();
    explicit DeferredMessage(Message& m);
    DeferredMessage(DeferredMessage&&);
    DeferredMessage& operator=(DeferredMessage&&);
    ~DeferredMessage();
    Message AsMessage();
    std::vector<uint8_t> data;
    std::vector<os::Handle> handles;
  };

  // on POSIX we use sendmsg() from arbitrary threads, which means the system is
  // free to interleave data from such calls if we aren't careful to prevent it.
  // that's what this is for.
  absl::Mutex send_mutex_;

  absl::Mutex queue_mutex_;
  std::vector<DeferredMessage> outgoing_queue_ ABSL_GUARDED_BY(queue_mutex_);
};

}  // namespace os
}  // namespace ipcz

#endif  // IPCZ_SRC_OS_CHANNEL_H_
