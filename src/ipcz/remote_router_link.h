// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_REMOTE_ROUTER_LINK_H_
#define IPCZ_SRC_IPCZ_REMOTE_ROUTER_LINK_H_

#include <atomic>

#include "ipcz/fragment_ref.h"
#include "ipcz/link_side.h"
#include "ipcz/link_type.h"
#include "ipcz/router_link.h"
#include "ipcz/router_link_state.h"
#include "ipcz/sublink_id.h"

namespace ipcz {

class NodeLink;

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
  // `link_state_fragment` is the shared memory span containing link's
  // RouterLinkState.
  static Ref<RemoteRouterLink> Create(
      Ref<NodeLink> node_link,
      SublinkId sublink,
      FragmentRef<RouterLinkState> link_state_fragment,
      LinkType type,
      LinkSide side);

  const Ref<NodeLink>& node_link() const { return node_link_; }
  SublinkId sublink() const { return sublink_; }

  void SetLinkState(FragmentRef<RouterLinkState> state);

  // RouterLink:
  LinkType GetType() const override;
  Ref<Router> GetLocalTarget() override;
  bool IsRemoteLinkTo(const NodeLink& node_link,
                      SublinkId sublink) const override;
  RouterLinkState::QueueState GetPeerQueueState() override;
  bool UpdateInboundQueueState(size_t num_bytes, size_t num_parcels) override;
  void MarkSideStable() override;
  bool TryLockForBypass(const NodeName& bypass_request_source) override;
  bool TryLockForClosure() override;
  void Unlock() override;
  void FlushOtherSideIfWaiting() override;
  bool CanNodeRequestBypass(const NodeName& bypass_request_source) override;
  bool WouldParcelExceedLimits(size_t data_size,
                               const IpczPutLimits& limits,
                               size_t* max_data_size) override;
  void AllocateParcelData(size_t num_bytes,
                          bool allow_partial,
                          Parcel& parcel) override;
  void AcceptParcel(Parcel& parcel) override;
  void AcceptRouteClosure(SequenceNumber sequence_length) override;
  void AcceptRouteDisconnection() override;
  void NotifyDataConsumed() override;
  bool SetSignalOnDataConsumed(bool signal) override;
  void RequestProxyBypassInitiation(const NodeName& to_new_peer,
                                    SublinkId proxy_peer_sublink) override;
  void StopProxying(SequenceNumber proxy_inbound_sequence_length,
                    SequenceNumber proxy_outbound_sequence_length) override;
  void ProxyWillStop(SequenceNumber proxy_inbound_sequence_length) override;
  void BypassProxyToSameNode(
      SublinkId new_sublink,
      FragmentRef<RouterLinkState> new_link_state,
      SequenceNumber proxy_inbound_sequence_length) override;
  void StopProxyingToLocalPeer(
      SequenceNumber proxy_outbound_sequence_length) override;
  void ShareLinkStateMemoryIfNecessary() override;
  void Deactivate() override;
  std::string Describe() const override;
  void LogRouteTrace() override;

 private:
  RemoteRouterLink(Ref<NodeLink> node_link,
                   SublinkId sublink,
                   FragmentRef<RouterLinkState> link_state_fragment,
                   LinkType type,
                   LinkSide side);

  ~RemoteRouterLink() override;

  void AllocateLinkState();

  RouterLinkState* GetLinkState() const;

  const Ref<NodeLink> node_link_;
  const SublinkId sublink_;
  const LinkType type_;
  const LinkSide side_;

  enum class LinkStatePhase {
    kNotPresent,
    kBusy,
    kPresent,
  };

  std::atomic<bool> must_share_link_state_fragment_{false};
  std::atomic<bool> side_is_stable_{false};
  std::atomic<LinkStatePhase> link_state_phase_{LinkStatePhase::kNotPresent};
  FragmentRef<RouterLinkState> link_state_fragment_;
  std::atomic<RouterLinkState*> link_state_{nullptr};
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_REMOTE_ROUTER_LINK_H_
