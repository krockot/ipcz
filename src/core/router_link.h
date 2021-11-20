// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTER_LINK_H_
#define IPCZ_SRC_CORE_ROUTER_LINK_H_

#include <cstddef>

#include "core/routing_id.h"
#include "core/sequence_number.h"
#include "core/side.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"

namespace ipcz {
namespace core {

class NodeLink;
class Parcel;
class Router;
struct RouterLinkState;

class RouterLink : public mem::RefCounted {
 public:
  virtual RouterLinkState& GetLinkState() = 0;
  virtual bool IsLocalLinkTo(Router& router) = 0;
  virtual bool IsRemoteLinkTo(NodeLink& node_link, RoutingId routing_id) = 0;
  virtual bool WouldParcelExceedLimits(size_t data_size,
                                       const IpczPutLimits& limits) = 0;
  virtual void AcceptParcel(Parcel& parcel) = 0;
  virtual void AcceptRouteClosure(Side side,
                                  SequenceNumber sequence_length) = 0;

 protected:
  ~RouterLink() override = default;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTER_LINK_H_
