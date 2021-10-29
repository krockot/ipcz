// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_TRAP_H_
#define IPCZ_SRC_CORE_TRAP_H_

#include <atomic>
#include <memory>

#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"

namespace ipcz {
namespace core {

class Trap {
 public:
  Trap(const IpczTrapConditions& conditions,
       IpczTrapEventHandler handler,
       uintptr_t context);
  ~Trap();

  bool is_armed() const { return is_armed_->load(std::memory_order_acquire); }

  void set_is_armed(bool armed) {
    is_armed_->store(armed, std::memory_order_release);
  }

  std::shared_ptr<std::atomic_bool> is_armed_flag() const { return is_armed_; }

  const IpczTrapConditions& conditions() const { return conditions_; }
  IpczTrapEventHandler handler() const { return handler_; }
  uintptr_t context() const { return context_; }

 private:
  const IpczTrapConditions conditions_;
  const IpczTrapEventHandler handler_;
  const uintptr_t context_;
  const std::shared_ptr<std::atomic_bool> is_armed_{
      std::make_shared<std::atomic_bool>(false)};
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_TRAP_H_
