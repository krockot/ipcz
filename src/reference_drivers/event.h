// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_REFERENCE_DRIVERS_EVENT_H_
#define IPCZ_SRC_REFERENCE_DRIVERS_EVENT_H_

#include "util/os_handle.h"

namespace ipcz {
namespace reference_drivers {

// An OS object which supports waiting and signaling across processes.
class Event {
 public:
  // An OS object which can signal an Event to stop waiting.
  class Notifier {
   public:
    Notifier();
    explicit Notifier(OSHandle handle);
    Notifier(Notifier&&);
    Notifier& operator=(Notifier&&);
    ~Notifier();

    bool is_valid() const { return handle_.is_valid(); }
    const OSHandle& handle() const { return handle_; }

    OSHandle TakeHandle();

    void Notify();
    Notifier Clone();

   private:
    OSHandle handle_;
  };

  Event();
  explicit Event(OSHandle handle);
  Event(Event&&);
  Event& operator=(Event&&);
  ~Event();

  bool is_valid() const { return handle_.is_valid(); }
  const OSHandle& handle() const { return handle_; }

  OSHandle TakeHandle();

  Notifier MakeNotifier();

  void Wait();

 private:
  OSHandle handle_;
};

}  // namespace reference_drivers
}  // namespace ipcz

#endif  // IPCZ_SRC_REFERENCE_DRIVERS_EVENT_H_
