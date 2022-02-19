// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/route_edge.h"

#include <string>

#include "ipcz/parcel_queue.h"
#include "ipcz/router.h"
#include "ipcz/router_link.h"
#include "util/log.h"

namespace ipcz {

RouteEdge::RouteEdge() = default;

RouteEdge::~RouteEdge() = default;

Ref<Router> RouteEdge::GetLocalPeer() const {
  if (!primary_link_) {
    return nullptr;
  }
  return primary_link_->GetLocalTarget();
}

Ref<Router> RouteEdge::GetDecayingLocalPeer() const {
  if (!decaying_link_) {
    return nullptr;
  }
  return decaying_link_->GetLocalTarget();
}

void RouteEdge::SetPrimaryLink(Ref<RouterLink> link) {
  if (was_decay_deferred_) {
    was_decay_deferred_ = false;
    decaying_link_ = std::move(link);
  } else {
    primary_link_ = std::move(link);
  }
}

Ref<RouterLink> RouteEdge::ReleasePrimaryLink() {
  return std::move(primary_link_);
}

Ref<RouterLink> RouteEdge::ReleaseDecayingLink() {
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

bool RouteEdge::TryLockPrimaryLinkForBypass(
    const NodeName& bypass_request_source) {
  if (decaying_link_ || was_decay_deferred_ || !primary_link_) {
    return false;
  }

  if (!primary_link_->GetType().is_central()) {
    return false;
  }

  return primary_link_->TryLockForBypass(bypass_request_source);
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

}  // namespace ipcz
