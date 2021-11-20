// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTER_OBSERVER_H_
#define IPCZ_SRC_CORE_ROUTER_OBSERVER_H_

#include <cstdint>

#include "mem/ref_counted.h"

namespace ipcz {
namespace core {

class RouterObserver : public mem::RefCounted {
 public:
  virtual void OnPeerClosed(bool is_route_dead) = 0;
  virtual void OnIncomingParcel(uint32_t num_available_parcels,
                                uint32_t num_avialable_bytes) = 0;

 protected:
  ~RouterObserver() override = default;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTER_OBSERVER_H_
