// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_OS_EVENT_H_
#define IPCZ_SRC_OS_EVENT_H_

#include "os/handle.h"

namespace ipcz {
namespace os {

// An OS object which supports waiting and signaling across processes.
class Event {
 public:
  // An OS object which can signal an Event to stop waiting.
  class Notifier {
   public:
    Notifier();
    explicit Notifier(Handle handle);
    Notifier(Notifier&&);
    Notifier& operator=(Notifier&&);
    ~Notifier();

    bool is_valid() const { return handle_.is_valid(); }
    const Handle& handle() const { return handle_; }

    Handle TakeHandle();

    void Notify();
    Notifier Clone();

   private:
    Handle handle_;
  };

  Event();
  explicit Event(Handle handle);
  Event(Event&&);
  Event& operator=(Event&&);
  ~Event();

  bool is_valid() const { return handle_.is_valid(); }
  const Handle& handle() const { return handle_; }

  Handle TakeHandle();

  Notifier MakeNotifier();

  void Wait();

 private:
  os::Handle handle_;
};

}  // namespace os
}  // namespace ipcz

#endif  // IPCZ_SRC_OS_EVENT_H_
