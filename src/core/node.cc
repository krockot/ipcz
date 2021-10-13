// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node.h"

#include <memory>
#include <utility>

#include "core/monitor.h"
#include "core/name.h"
#include "core/portal.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {
namespace core {

Node::Node() = default;

Node::~Node() = default;

Portal::Pair Node::OpenPortals() {
  return Portal::CreateLocalPair(mem::WrapRefCounted(this));
}

IpczResult Node::OpenRemotePortal(os::Channel channel,
                                  os::Process process,
                                  std::unique_ptr<Portal>& out_portal) {
  out_portal = Portal::CreateRouted(mem::WrapRefCounted(this));
  return IPCZ_RESULT_OK;
}

IpczResult Node::AcceptRemotePortal(os::Channel channel,
                                    std::unique_ptr<Portal>& out_portal) {
  out_portal = Portal::CreateRouted(mem::WrapRefCounted(this));
  return IPCZ_RESULT_OK;
}

}  // namespace core
}  // namespace ipcz
