// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTER_H_
#define IPCZ_SRC_CORE_ROUTER_H_

#include "core/name.h"

namespace ipcz {
namespace core {

class Parcel;

// An interface implemented privately by Node and exposed only via a
// LockedRouter, which holds the Node's internal lock throughout its lifetime.
// This enables certain Portal entry points to call into the Node's Router
// interface under the safe assumption that the Node's internal lock is already
// held.
class Router {
 public:
  virtual bool RouteParcel(const PortalAddress& destination,
                           Parcel& parcel) = 0;
  virtual bool NotifyPeerClosed(const PortalAddress& destination) = 0;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTER_H_
