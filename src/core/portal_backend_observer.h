// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_BACKEND_OBSERVER_H_
#define IPCZ_SRC_CORE_PORTAL_BACKEND_OBSERVER_H_

#include "ipcz/ipcz.h"

namespace ipcz {
namespace core {

// Copy of portal status information provided to PortalBackendObserver
// notifications.
struct PortalBackendStatus {
  IpczPortalStatusBits bits;
  uint64_t num_local_parcels;
  uint64_t num_local_bytes;
  uint64_t num_remote_parcels;
  uint64_t num_remote_bytes;
};

// Interface used by a Portal to monitor its backend for interesting events.
class PortalBackendObserver {
 public:
  virtual ~PortalBackendObserver() = default;

  virtual void OnPeerClosed(const PortalBackendStatus& status) = 0;
  virtual void OnPortalDead(const PortalBackendStatus& status) = 0;
  virtual void OnQueueChanged(const PortalBackendStatus& status) = 0;
  virtual void OnPeerQueueChanged(const PortalBackendStatus& status) = 0;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_BACKEND_OBSERVER_H_
