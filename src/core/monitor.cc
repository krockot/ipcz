// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/monitor.h"

namespace ipcz {
namespace core {

Monitor::Monitor() = default;

Monitor::~Monitor() = default;

IpczResult Monitor::Activate(IpczMonitorConditionFlags* conditions,
                             IpczPortalStatus* status) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

}  // namespace core
}  // namespace ipcz
