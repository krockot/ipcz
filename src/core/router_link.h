// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTER_LINK_H_
#define IPCZ_SRC_CORE_ROUTER_LINK_H_

#include <cstddef>

#include "core/node_name.h"
#include "core/routing_id.h"
#include "core/sequence_number.h"
#include "core/side.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/numeric/int128.h"

namespace ipcz {
namespace core {

class NodeLink;
class Parcel;
class Router;
struct RouterLinkState;

class RouterLink : public mem::RefCounted {
 public:
  virtual void Deactivate() = 0;
  virtual RouterLinkState& GetLinkState() = 0;
  virtual mem::Ref<Router> GetLocalTarget() = 0;
  virtual bool IsRemoteLinkTo(NodeLink& node_link, RoutingId routing_id) = 0;
  virtual bool IsLinkToOtherSide() = 0;
  virtual bool WouldParcelExceedLimits(size_t data_size,
                                       const IpczPutLimits& limits) = 0;
  virtual void AcceptParcel(Parcel& parcel) = 0;
  virtual void AcceptRouteClosure(Side side,
                                  SequenceNumber sequence_length) = 0;
  virtual void StopProxying(SequenceNumber inbound_sequence_length,
                            SequenceNumber outbound_sequence_length) = 0;
  virtual void RequestProxyBypassInitiation(
      const NodeName& to_new_peer,
      RoutingId proxy_peer_routing_id,
      const absl::uint128& bypass_key) = 0;
  virtual void BypassProxyToSameNode(RoutingId new_routing_id,
                                     SequenceNumber sequence_length) = 0;
  virtual void StopProxyingToLocalPeer(SequenceNumber sequence_length) = 0;

 protected:
  ~RouterLink() override = default;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTER_LINK_H_
