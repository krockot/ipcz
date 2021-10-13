// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_MONITOR_H_
#define IPCZ_SRC_CORE_MONITOR_H_

#include "ipcz/ipcz.h"

namespace ipcz {
namespace core {

class Monitor {
 public:
  Monitor();
  ~Monitor();

  IpczResult Activate(IpczMonitorConditionFlags* conditions,
                      IpczPortalStatus* status);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_MONITOR_H_
