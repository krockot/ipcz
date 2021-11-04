// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/incoming_parcel_queue.h"

#include <algorithm>
#include <utility>

#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace core {

IncomingParcelQueue::IncomingParcelQueue() = default;

IncomingParcelQueue::IncomingParcelQueue(SequenceNumber first_sequence_number)
    : base_sequence_number_(first_sequence_number) {}

IncomingParcelQueue::IncomingParcelQueue(IncomingParcelQueue&& other)
    : base_sequence_number_(other.base_sequence_number_),
      num_parcels_(other.num_parcels_),
      highest_expected_sequence_number_(
          other.highest_expected_sequence_number_) {
  if (!other.storage_.empty()) {
    size_t parcels_offset = other.parcels_.data() - storage_.data();
    storage_ = std::move(other.storage_);
    parcels_ =
        ParcelSpan(storage_.data() + parcels_offset, other.parcels_.size());
  }
}

IncomingParcelQueue& IncomingParcelQueue::operator=(
    IncomingParcelQueue&& other) {
  base_sequence_number_ = other.base_sequence_number_;
  num_parcels_ = other.num_parcels_;
  highest_expected_sequence_number_ = other.highest_expected_sequence_number_;
  if (!other.storage_.empty()) {
    size_t parcels_offset = other.parcels_.data() - storage_.data();
    storage_ = std::move(other.storage_);
    parcels_ =
        ParcelSpan(storage_.data() + parcels_offset, other.parcels_.size());
  } else {
    storage_.clear();
    parcels_ = ParcelSpan(storage_.data(), 0);
  }
  return *this;
}

IncomingParcelQueue::~IncomingParcelQueue() = default;

size_t IncomingParcelQueue::GetSize() const {
  return parcels_.size();
}

bool IncomingParcelQueue::SetHighestExpectedSequenceNumber(SequenceNumber n) {
  if (highest_expected_sequence_number_) {
    return false;
  }

  if (n < base_sequence_number_ + parcels_.size()) {
    return false;
  }

  highest_expected_sequence_number_ = n;
  Reallocate(n);
  return true;
}

bool IncomingParcelQueue::IsExpectingMoreParcels() const {
  if (!highest_expected_sequence_number_) {
    return true;
  }

  if (base_sequence_number_ > *highest_expected_sequence_number_) {
    return false;
  }

  const size_t num_parcels_remaining =
      *highest_expected_sequence_number_ - base_sequence_number_ + 1;
  return num_parcels_ < num_parcels_remaining;
}

bool IncomingParcelQueue::HasNextParcel() const {
  return !parcels_.empty() && parcels_[0].has_value();
}

bool IncomingParcelQueue::Push(Parcel& parcel) {
  const SequenceNumber n = parcel.sequence_number();
  if (n < base_sequence_number_) {
    return false;
  }

  size_t index = n - base_sequence_number_;
  if (highest_expected_sequence_number_) {
    if (index >= parcels_.size() || parcels_[index].has_value()) {
      return false;
    }
    parcels_[index] = std::move(parcel);
    ++num_parcels_;
    return true;
  }

  if (index < parcels_.size()) {
    if (parcels_[index].has_value()) {
      return false;
    }
    parcels_[index] = std::move(parcel);
    ++num_parcels_;
    return true;
  }

  Reallocate(n);
  ABSL_ASSERT(index < parcels_.size());
  parcels_[index] = std::move(parcel);
  ++num_parcels_;
  return true;
}

bool IncomingParcelQueue::Pop(Parcel& parcel) {
  if (parcels_.empty() || !parcels_[0].has_value()) {
    return false;
  }

  parcel = std::move(*parcels_[0]);
  parcels_[0].reset();
  parcels_ = parcels_.subspan(1);

  ABSL_ASSERT(num_parcels_ > 0);
  --num_parcels_;
  ++base_sequence_number_;

  // If there's definitely no more populated parcel data, take this opporunity
  // to realign `parcels_` to the front of `storage_` to reduce future
  // allocations.
  if (num_parcels_ == 0) {
    parcels_ = ParcelSpan(storage_.data(), parcels_.size());
  }

  return true;
}

Parcel& IncomingParcelQueue::NextParcel() {
  ABSL_ASSERT(HasNextParcel());
  return *parcels_[0];
}

void IncomingParcelQueue::Reallocate(SequenceNumber max_sequence_number) {
  size_t parcels_offset = parcels_.data() - storage_.data();
  size_t new_parcels_size = max_sequence_number - base_sequence_number_ + 1;
  if (parcels_offset + new_parcels_size < storage_.size()) {
    // Fast path: just extend the view into storage.
    parcels_ = ParcelSpan(storage_.data() + parcels_offset, new_parcels_size);
    return;
  }

  // We need to reallocate storage. Re-align `parcels_` with the front of the
  // buffer, and leave some extra room when allocating.
  if (parcels_offset > 0) {
    std::move(parcels_.begin(), parcels_.end(), storage_.begin());
  }
  storage_.resize(new_parcels_size * 2);
  parcels_ = ParcelSpan(storage_.data(), new_parcels_size);
}

}  // namespace core
}  // namespace ipcz
