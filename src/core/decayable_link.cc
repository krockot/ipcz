// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/decayable_link.h"

#include <utility>

#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace core {

DecayableLink::DecayableLink() = default;

DecayableLink::~DecayableLink() = default;

void DecayableLink::ResetCurrentLink() {
  current_link_.reset();
}

void DecayableLink::ResetDecayingLink() {
  decaying_link_.reset();
  length_to_decaying_link_.reset();
  length_from_decaying_link_.reset();
}

mem::Ref<Router> DecayableLink::GetLocalPeer() const {
  return current_link_ ? current_link_->GetLocalTarget() : nullptr;
}

mem::Ref<Router> DecayableLink::GetDecayingLocalPeer() const {
  return decaying_link_ ? decaying_link_->GetLocalTarget() : nullptr;
}

SequenceNumber DecayableLink::SetCurrentLink(mem::Ref<RouterLink> link) {
  current_link_ = std::move(link);
  return length_to_decaying_link_.value_or(parcels_.current_sequence_number());
}

mem::Ref<RouterLink> DecayableLink::TakeCurrentLink() {
  return std::move(current_link_);
}

mem::Ref<RouterLink> DecayableLink::TakeCurrentOrDecayingLink() {
  return current_link_ ? std::move(current_link_) : std::move(decaying_link_);
}

bool DecayableLink::AllowDecay() {
  ABSL_ASSERT(current_link_);
  return current_link_->SetSideCanDecay();
}

void DecayableLink::StartDecayingWithLink(
    mem::Ref<RouterLink> link,
    absl::optional<SequenceNumber> length_to_link,
    absl::optional<SequenceNumber> length_from_link) {
  ABSL_ASSERT(!decaying_link_);
  decaying_link_ = std::move(link);
  length_to_decaying_link_ = length_to_link;
  length_from_decaying_link_ = length_from_link;
}

void DecayableLink::StartDecaying(
    absl::optional<SequenceNumber> length_to_link,
    absl::optional<SequenceNumber> length_from_link) {
  StartDecayingWithLink(std::move(current_link_), length_to_link,
                        length_from_link);
}

void DecayableLink::FlushParcels(
    absl::InlinedVector<Parcel, 2>& parcels_to_decaying_link,
    absl::InlinedVector<Parcel, 2>& parcels_to_current_link) {
  Parcel parcel;
  while (parcels_.HasNextParcel() &&
         ShouldSendOnDecayingLink(parcels_.current_sequence_number())) {
    bool ok = parcels_.Pop(parcel);
    ABSL_ASSERT(ok);

    parcels_to_decaying_link.push_back(std::move(parcel));
  }

  while (!ShouldSendOnDecayingLink(parcels_.current_sequence_number()) &&
         current_link_ && parcels_.Pop(parcel)) {
    parcels_to_current_link.push_back(std::move(parcel));
  }
}

bool DecayableLink::IsDecayFinished(
    SequenceNumber received_sequence_length) const {
  return has_decaying_link() && length_to_decaying_link_ &&
         length_from_decaying_link_ &&
         parcels_.current_sequence_number() >= *length_to_decaying_link_ &&
         received_sequence_length >= *length_from_decaying_link_;
}

bool DecayableLink::ShouldSendOnDecayingLink(SequenceNumber n) const {
  return has_decaying_link() &&
         (!length_to_decaying_link_ || n < *length_to_decaying_link_);
}

}  // namespace core
}  // namespace ipcz
