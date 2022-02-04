// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_REMOTE_ROUTER_LINK_H_
#define IPCZ_SRC_CORE_REMOTE_ROUTER_LINK_H_

#include <atomic>

#include "core/link_side.h"
#include "core/link_type.h"
#include "core/node_link_address.h"
#include "core/router_link.h"
#include "core/sublink_id.h"

namespace ipcz {
namespace core {

class NodeLink;
struct RouterLinkState;

// One side of a link between two Routers living on different nodes. A
// RemoteRouterLink uses a NodeLink plus a SublinkId as its transport between
// the routers. On the other end (on another node) is another RemoteRouterLink
// using a NodeLink back to this node, with the same SublinkId.
//
// As with other RouterLink instances, each RemoteRouterLink is assigned a
// LinkSide at construction time. This assignment is arbitrary but will always
// be the opposite of the LinkSide assigned to the RemoteRouteLink on the other
// end.
class RemoteRouterLink : public RouterLink {
 public:
  // Constructs a new RemoteRouterLink which sends messages over `node_link`
  // using `sublink` specifically. `side` is the side of this link on which
  // this RemoteRouterLink falls (side A or B), and `type` indicates what type
  // of link it is -- which for remote links must be either kCentral,
  // kPeripheralInward, or kPeripheralOutward.
  //
  // `link_state_address` is the shared memory location of this link's
  // RouterLinkState.
  static mem::Ref<RemoteRouterLink> Create(
      mem::Ref<NodeLink> node_link,
      SublinkId sublink,
      const NodeLinkAddress& link_state_address,
      LinkType type,
      LinkSide side);

  const mem::Ref<NodeLink>& node_link() const { return node_link_; }
  SublinkId sublink() const { return sublink_; }
  NodeLinkAddress link_state_address() const { return link_state_address_; }

  void SetLinkStateAddress(const NodeLinkAddress& address);

  // RouterLink:
  LinkType GetType() const override;
  mem::Ref<Router> GetLocalTarget() override;
  bool IsRemoteLinkTo(NodeLink& node_link, SublinkId sublink) override;
  void MarkSideStable() override;
  bool TryLockForBypass(const NodeName& bypass_request_source) override;
  bool TryLockForClosure() override;
  void Unlock() override;
  void FlushOtherSideIfWaiting() override;
  bool CanNodeRequestBypass(const NodeName& bypass_request_source) override;
  bool WouldParcelExceedLimits(size_t data_size,
                               const IpczPutLimits& limits) override;
  void AcceptParcel(Parcel& parcel) override;
  void AcceptRouteClosure(SequenceNumber sequence_length) override;
  void RequestProxyBypassInitiation(const NodeName& to_new_peer,
                                    SublinkId proxy_peer_sublink) override;
  void StopProxying(SequenceNumber proxy_inbound_sequence_length,
                    SequenceNumber proxy_outbound_sequence_length) override;
  void ProxyWillStop(SequenceNumber proxy_inbound_sequence_length) override;
  void BypassProxyToSameNode(
      SublinkId new_sublink,
      const NodeLinkAddress& new_link_state_address,
      SequenceNumber proxy_inbound_sequence_length) override;
  void StopProxyingToLocalPeer(
      SequenceNumber proxy_outbound_sequence_length) override;
  void ShareLinkStateMemoryIfNecessary() override;
  void Deactivate() override;
  std::string Describe() const override;
  void LogRouteTrace() override;

 private:
  RemoteRouterLink(mem::Ref<NodeLink> node_link,
                   SublinkId sublink,
                   const NodeLinkAddress& link_state_address,
                   LinkType type,
                   LinkSide side);

  ~RemoteRouterLink() override;

  void AllocateLinkState();

  RouterLinkState* GetLinkState() const;

  const mem::Ref<NodeLink> node_link_;
  const SublinkId sublink_;
  const LinkType type_;
  const LinkSide side_;

  enum class LinkStatePhase {
    kNotPresent,
    kBusy,
    kPresent,
  };

  std::atomic<bool> must_share_link_state_address_{false};
  std::atomic<bool> side_is_stable_{false};
  std::atomic<LinkStatePhase> link_state_phase_{LinkStatePhase::kNotPresent};
  NodeLinkAddress link_state_address_;
  std::atomic<RouterLinkState*> link_state_{nullptr};
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_REMOTE_ROUTER_LINK_H_
