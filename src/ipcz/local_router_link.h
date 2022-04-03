// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_LOCAL_ROUTER_LINK_H_
#define IPCZ_SRC_IPCZ_LOCAL_ROUTER_LINK_H_

#include <utility>

#include "ipcz/link_side.h"
#include "ipcz/router.h"
#include "ipcz/router_link.h"
#include "ipcz/router_link_state.h"
#include "util/ref_counted.h"

namespace ipcz {

// Local link between two Routers on the same node. A LocalRouterLink is always
// a central link. Several RouterLink overrides are unimplemented by
// LocalRouterLink as they are unnecessary and unused for local links.
class LocalRouterLink : public RouterLink {
 public:
  enum class InitialState {
    kCannotBypass,
    kCanBypass,
  };

  // Creates a new pair of LocalRouterLinks with the given initial link status
  // and linking the given pair of Routers together. The Routers must not
  // currently have outward links.
  static RouterLink::Pair CreatePair(LinkType type,
                                     InitialState initial_state,
                                     const Router::Pair& routers);

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
  class SharedState;

  LocalRouterLink(LinkSide side, Ref<SharedState> state);
  ~LocalRouterLink() override;

  const LinkSide side_;
  const Ref<SharedState> state_;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_LOCAL_ROUTER_LINK_H_
