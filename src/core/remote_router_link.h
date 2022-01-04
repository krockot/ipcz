// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_REMOTE_ROUTER_LINK_H_
#define IPCZ_SRC_CORE_REMOTE_ROUTER_LINK_H_

#include "core/link_side.h"
#include "core/link_type.h"
#include "core/node_link_address.h"
#include "core/router_link.h"
#include "core/routing_id.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace ipcz {
namespace core {

class NodeLink;
struct RouterLinkState;

// One side of a link between two Routers living on different nodes. A
// RemoteRouterLink uses a NodeLink plus a RoutingId as its transport between
// the routers. On the other end (on another node) is another RemoteRouterLink
// using a NodeLink back to this node, with the same RoutingId.
//
// As with other RouterLink instances, each RemoteRouterLink is assigned a
// LinkSide at construction time. This assignment is arbitrary but will always
// be the opposite of the LinkSide assigned to the RemoteRouteLink on the other
// end.
class RemoteRouterLink : public RouterLink {
 public:
  // Constructs a new RemoteRouterLink which sends messages over `node_link`
  // using `routing_id` specifically. `side` is the side of this link on which
  // this RemoteRouterLink falls (side A or B), and `type` indicates what type
  // of link it is -- which for remote links must be either kCentral,
  // kPeripheralInward, or kPeripheralOutward.
  //
  // `link_state_address` is the shared memory location of this link's
  // RouterLinkState.
  RemoteRouterLink(mem::Ref<NodeLink> node_link,
                   RoutingId routing_id,
                   absl::optional<NodeLinkAddress> link_state_address,
                   LinkType type,
                   LinkSide side);

  const mem::Ref<NodeLink>& node_link() const { return node_link_; }
  RoutingId routing_id() const { return routing_id_; }
  absl::optional<NodeLinkAddress> link_state_address() const {
    return link_state_address_;
  }

  // RouterLink:
  LinkType GetType() const override;
  mem::Ref<Router> GetLocalTarget() override;
  bool IsRemoteLinkTo(NodeLink& node_link, RoutingId routing_id) override;
  bool CanLockForBypass() override;
  bool SetSideCanSupportBypass() override;
  bool TryToLockForBypass(const NodeName& bypass_request_source) override;
  bool CancelBypassLock() override;
  bool CanNodeRequestBypass(const NodeName& bypass_request_source) override;
  bool WouldParcelExceedLimits(size_t data_size,
                               const IpczPutLimits& limits) override;
  void AcceptParcel(Parcel& parcel) override;
  void AcceptRouteClosure(SequenceNumber sequence_length) override;
  void RequestProxyBypassInitiation(const NodeName& to_new_peer,
                                    RoutingId proxy_peer_routing_id) override;
  void StopProxying(SequenceNumber proxy_inbound_sequence_length,
                    SequenceNumber proxy_outbound_sequence_length) override;
  void ProxyWillStop(SequenceNumber proxy_inbound_sequence_length) override;
  void BypassProxyToSameNode(
      RoutingId new_routing_id,
      const NodeLinkAddress& new_link_state_address,
      SequenceNumber proxy_inbound_sequence_length) override;
  void StopProxyingToLocalPeer(
      SequenceNumber proxy_outbound_sequence_length) override;
  void NotifyBypassPossible() override;
  void Deactivate() override;
  std::string Describe() const override;
  void LogRouteTrace() override;

 private:
  ~RemoteRouterLink() override;

  RouterLinkState* GetLinkState();

  const mem::Ref<NodeLink> node_link_;
  const RoutingId routing_id_;
  const LinkType type_;
  const LinkSide side_;

  absl::optional<NodeLinkAddress> link_state_address_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_REMOTE_ROUTER_LINK_H_
