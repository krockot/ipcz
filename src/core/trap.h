// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_TRAP_H_
#define IPCZ_SRC_CORE_TRAP_H_

#include "ipcz/ipcz.h"

namespace ipcz {
namespace core {

class Trap {
 public:
  Trap(const IpczTrapConditions& conditions,
       IpczTrapEventHandler handler,
       uintptr_t context);
  ~Trap();

  const IpczTrapConditions& conditions() const { return conditions_; }
  IpczTrapEventHandler handler() const { return handler_; }
  uintptr_t context() const { return context_; }

 private:
  const IpczTrapConditions conditions_;
  const IpczTrapEventHandler handler_;
  const uintptr_t context_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_TRAP_H_
