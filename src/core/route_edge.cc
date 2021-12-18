// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/route_edge.h"

#include <string>

#include "core/parcel_queue.h"
#include "core/router.h"
#include "core/router_link.h"
#include "debug/log.h"

namespace ipcz {
namespace core {

RouteEdge::RouteEdge() = default;

RouteEdge::~RouteEdge() = default;

mem::Ref<Router> RouteEdge::GetLocalPeer() const {
  if (!primary_link_) {
    return nullptr;
  }
  return primary_link_->GetLocalTarget();
}

mem::Ref<Router> RouteEdge::GetDecayingLocalPeer() const {
  if (!decaying_link_) {
    return nullptr;
  }
  return decaying_link_->GetLocalTarget();
}

void RouteEdge::SetPrimaryLink(mem::Ref<RouterLink> link) {
  if (was_decay_deferred_) {
    was_decay_deferred_ = false;
    decaying_link_ = std::move(link);
  } else {
    primary_link_ = std::move(link);
  }
}

mem::Ref<RouterLink> RouteEdge::ReleasePrimaryLink() {
  return std::move(primary_link_);
}

mem::Ref<RouterLink> RouteEdge::ReleaseDecayingLink() {
  return std::move(decaying_link_);
}

void RouteEdge::FlushParcelsFromQueue(
    ParcelQueue& parcels,
    FlushedParcelQueue& parcels_to_decaying_link,
    FlushedParcelQueue& parcels_to_primary_link) {
  Parcel parcel;
  while (parcels.HasNextParcel() && decaying_link_ &&
         ShouldSendOnDecayingLink(parcels.current_sequence_number())) {
    bool ok = parcels.Pop(parcel);
    ABSL_ASSERT(ok);

    parcels_to_decaying_link.push_back(std::move(parcel));
  }

  while (!ShouldSendOnDecayingLink(parcels.current_sequence_number()) &&
         primary_link_ && parcels.Pop(parcel)) {
    parcels_to_primary_link.push_back(std::move(parcel));
  }
}

bool RouteEdge::SetPrimaryLinkCanSupportBypass() {
  // Only central links are responsible for blocking or unblocking bypass.
  ABSL_ASSERT(primary_link_);
  ABSL_ASSERT(primary_link_->GetType().is_central());
  return primary_link_->SetSideCanSupportBypass();
}

bool RouteEdge::CanLockPrimaryLinkForBypass() {
  if (!primary_link_ || decaying_link_ || was_decay_deferred_) {
    return false;
  }

  if (!primary_link_->GetType().is_central()) {
    return false;
  }

  return primary_link_->CanLockForBypass();
}

bool RouteEdge::TryToLockPrimaryLinkForBypass(
    const NodeName& bypass_request_source) {
  if (decaying_link_ || was_decay_deferred_ || !primary_link_) {
    return false;
  }

  if (!primary_link_->GetType().is_central()) {
    return false;
  }

  return primary_link_->TryToLockForBypass(bypass_request_source);
}

bool RouteEdge::CanNodeRequestBypassOfPrimaryLink(
    const NodeName& bypass_request_source) {
  if (!primary_link_ || !primary_link_->GetType().is_central()) {
    return false;
  }
  return primary_link_->CanNodeRequestBypass(bypass_request_source);
}

bool RouteEdge::StartDecaying(
    absl::optional<SequenceNumber> length_to_decaying_link,
    absl::optional<SequenceNumber> length_from_decaying_link) {
  if (length_to_decaying_link_ || length_from_decaying_link_ ||
      decaying_link_ || was_decay_deferred_) {
    return false;
  }

  length_to_decaying_link_ = length_to_decaying_link;
  length_from_decaying_link_ = length_from_decaying_link;
  decaying_link_ = std::move(primary_link_);
  was_decay_deferred_ = !decaying_link_;
  return true;
}

bool RouteEdge::TryToFinishDecaying(SequenceNumber sequence_length_sent,
                                    SequenceNumber sequence_length_received) {
  if (!decaying_link_) {
    return false;
  }

  if (!length_to_decaying_link_) {
    DVLOG(4) << "Cannot decay yet with no known sequence length to "
             << decaying_link_->Describe();
    return false;
  }

  if (!length_from_decaying_link_) {
    DVLOG(4) << "Cannot decay yet with no known sequence length to "
             << decaying_link_->Describe();
    return false;
  }

  if (sequence_length_sent < *length_to_decaying_link_) {
    DVLOG(4) << "Cannot decay yet without sending full sequence up to "
             << *length_to_decaying_link_ << " on "
             << decaying_link_->Describe();
    return false;
  }

  if (sequence_length_received < *length_from_decaying_link_) {
    DVLOG(4) << "Cannot decay yet without receiving full sequence up to "
             << *length_from_decaying_link_ << " on "
             << decaying_link_->Describe();
    return false;
  }

  ABSL_ASSERT(!was_decay_deferred_);
  decaying_link_.reset();
  length_to_decaying_link_.reset();
  length_from_decaying_link_.reset();
  return true;
}

mem::Ref<RouterLink> RouteEdge::GetLinkToPropagateRouteClosure() {
  if (closure_propagated_ || (!primary_link_ && !decaying_link_)) {
    return nullptr;
  }

  closure_propagated_ = true;
  return primary_link_ ? primary_link_ : decaying_link_;
}

void RouteEdge::LogDescription() const {
  if (primary_link_) {
    DLOG(INFO) << "   - primary link: " << primary_link_->Describe();
  } else {
    DLOG(INFO) << "   - primary link: (none)";
  }

  if (decaying_link_) {
    DLOG(INFO) << "   - decaying link: " << decaying_link_->Describe();
  } else {
    DLOG(INFO) << "   - decaying link: (none)";
  }

  if (length_to_decaying_link_) {
    DLOG(INFO) << "   - sequence length to decaying link: "
               << *length_to_decaying_link_;
  } else {
    DLOG(INFO) << "   - sequence length to decaying link: (none)";
  }

  if (length_from_decaying_link_) {
    DLOG(INFO) << "   - sequence length from decaying link: "
               << *length_from_decaying_link_;
  } else {
    DLOG(INFO) << "   - sequence length from decaying link: (none)";
  }
}

bool RouteEdge::ShouldSendOnDecayingLink(SequenceNumber n) const {
  return (decaying_link_ || was_decay_deferred_) &&
         (!length_to_decaying_link_ || n < *length_to_decaying_link_);
}

}  // namespace core
}  // namespace ipcz
