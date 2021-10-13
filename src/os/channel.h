// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_OS_CHANNEL_H_
#define IPCZ_SRC_OS_CHANNEL_H_

#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

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

  Handle TakeHandle();

  using MessageHandler = std::function<void(Message)>;
  void Listen(MessageHandler handler);

  void StopListening();

  void Reset();

  bool Send(Message message);

 private:
  void ReadMessagesOnIOThread(MessageHandler handler, Event shutdown_event);

  Handle handle_;
  Event::Notifier shutdown_notifier_;
  absl::optional<std::thread> io_thread_;
};

}  // namespace os
}  // namespace ipcz

#endif  // IPCZ_SRC_OS_CHANNEL_H_
