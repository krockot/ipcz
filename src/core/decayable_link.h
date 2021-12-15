// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_DECAYABLE_LINK_H_
#define IPCZ_SRC_CORE_DECAYABLE_LINK_H_

#include <cstddef>

#include "core/node_name.h"
#include "core/parcel_queue.h"
#include "core/router_link.h"
#include "core/sequence_number.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace ipcz {
namespace core {

class Router;

class DecayableLink {
 public:
  DecayableLink();
  DecayableLink(const DecayableLink&) = delete;
  DecayableLink& operator=(const DecayableLink&) = delete;
  ~DecayableLink();

  bool has_current_link() const { return current_link_ != nullptr; }
  const mem::Ref<RouterLink>& current_link() const { return current_link_; }

  bool has_decaying_link() const { return decaying_link_ != nullptr; }
  const mem::Ref<RouterLink>& decaying_link() const { return decaying_link_; }

  bool has_any_link() const {
    return has_current_link() || has_decaying_link();
  }

  ParcelQueue& parcels() { return parcels_; }
  const ParcelQueue& parcels() const { return parcels_; }

  bool closure_propagated() const { return closure_propagated_; }
  void set_closure_propagated(bool propagated) {
    closure_propagated_ = propagated;
  }

  SequenceNumber sequence_length() const {
    return parcels_.GetCurrentSequenceLength();
  }

  SequenceNumber current_sequence_number() const {
    return parcels_.current_sequence_number();
  }

  absl::optional<SequenceNumber> length_to_decaying_link() const {
    return length_to_decaying_link_;
  }
  void set_length_to_decaying_link(SequenceNumber length) {
    length_to_decaying_link_ = length;
  }

  absl::optional<SequenceNumber> length_from_decaying_link() const {
    return length_from_decaying_link_;
  }
  void set_length_from_decaying_link(SequenceNumber length) {
    length_from_decaying_link_ = length;
  }

  void ResetCurrentLink();
  void ResetDecayingLink();
  mem::Ref<Router> GetLocalPeer() const;
  mem::Ref<Router> GetDecayingLocalPeer() const;
  void SetCurrentLink(mem::Ref<RouterLink> link);
  mem::Ref<RouterLink> TakeCurrentLink();
  mem::Ref<RouterLink> TakeCurrentOrDecayingLink();
  bool UnblockDecay();
  bool TryToDecay(const NodeName& bypass_request_source);
  void StartDecayingWithLink(
      mem::Ref<RouterLink> link,
      absl::optional<SequenceNumber> length_to_link = absl::nullopt,
      absl::optional<SequenceNumber> length_from_link = absl::nullopt);
  void StartDecaying(
      absl::optional<SequenceNumber> length_to_link = absl::nullopt,
      absl::optional<SequenceNumber> length_from_link = absl::nullopt);
  void FlushParcels(absl::InlinedVector<Parcel, 2>& parcels_to_decaying_link,
                    absl::InlinedVector<Parcel, 2>& parcels_to_current_link);
  bool IsDecayFinished(SequenceNumber received_sequence_length) const;
  bool ShouldPropagateRouteClosure() const;

 private:
  bool ShouldSendOnDecayingLink(SequenceNumber n) const;

  ParcelQueue parcels_;
  mem::Ref<RouterLink> current_link_;
  mem::Ref<RouterLink> decaying_link_;
  absl::optional<SequenceNumber> length_to_decaying_link_;
  absl::optional<SequenceNumber> length_from_decaying_link_;
  bool closure_propagated_ = false;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_DECAYABLE_LINK_H_
