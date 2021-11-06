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

IncomingParcelQueue::IncomingParcelQueue(SequenceNumber current_sequence_number)
    : base_sequence_number_(current_sequence_number) {}

IncomingParcelQueue::IncomingParcelQueue(IncomingParcelQueue&& other)
    : base_sequence_number_(other.base_sequence_number_),
      num_parcels_(other.num_parcels_),
      peer_sequence_length_(other.peer_sequence_length_) {
  if (!other.storage_.empty()) {
    size_t parcels_offset = other.parcels_.data() - storage_.data();
    storage_ = std::move(other.storage_);
    parcels_ =
        ParcelView(storage_.data() + parcels_offset, other.parcels_.size());
  }
}

IncomingParcelQueue& IncomingParcelQueue::operator=(
    IncomingParcelQueue&& other) {
  base_sequence_number_ = other.base_sequence_number_;
  num_parcels_ = other.num_parcels_;
  peer_sequence_length_ = other.peer_sequence_length_;
  if (!other.storage_.empty()) {
    size_t parcels_offset = other.parcels_.data() - storage_.data();
    storage_ = std::move(other.storage_);
    parcels_ =
        ParcelView(storage_.data() + parcels_offset, other.parcels_.size());
  } else {
    storage_.clear();
    parcels_ = ParcelView(storage_.data(), 0);
  }
  return *this;
}

IncomingParcelQueue::~IncomingParcelQueue() = default;

size_t IncomingParcelQueue::GetNumAvailableParcels() const {
  if (parcels_.empty() || !parcels_[0].has_value()) {
    return 0;
  }

  return parcels_[0]->num_parcels_in_span;
}

size_t IncomingParcelQueue::GetNumAvailableBytes() const {
  if (parcels_.empty() || !parcels_[0].has_value()) {
    return 0;
  }

  return parcels_[0]->num_bytes_in_span;
}

bool IncomingParcelQueue::SetPeerSequenceLength(SequenceNumber length) {
  if (peer_sequence_length_) {
    return false;
  }

  if (length < base_sequence_number_ + parcels_.size()) {
    return false;
  }

  peer_sequence_length_ = length;
  Reallocate(length);
  return true;
}

bool IncomingParcelQueue::IsExpectingMoreParcels() const {
  if (!peer_sequence_length_) {
    return true;
  }

  if (base_sequence_number_ >= *peer_sequence_length_) {
    return false;
  }

  const size_t num_parcels_remaining =
      *peer_sequence_length_ - base_sequence_number_;
  return num_parcels_ < num_parcels_remaining;
}

absl::optional<SequenceNumber>
IncomingParcelQueue::GetNextExpectedSequenceNumber() const {
  if (!IsExpectingMoreParcels()) {
    return absl::nullopt;
  }
  return base_sequence_number_ + parcels_.size();
}

bool IncomingParcelQueue::HasNextParcel() const {
  return !parcels_.empty() && parcels_[0].has_value();
}

bool IncomingParcelQueue::Push(Parcel parcel) {
  const SequenceNumber n = parcel.sequence_number();
  if (n < base_sequence_number_) {
    return false;
  }

  size_t index = n - base_sequence_number_;
  if (peer_sequence_length_) {
    if (index >= parcels_.size() || parcels_[index].has_value()) {
      return false;
    }
    PlaceNewEntry(index, parcel);
    return true;
  }

  if (index < parcels_.size()) {
    if (parcels_[index].has_value()) {
      return false;
    }
    PlaceNewEntry(index, parcel);
    return true;
  }

  Reallocate(n + 1);
  PlaceNewEntry(index, parcel);
  return true;
}

bool IncomingParcelQueue::Pop(Parcel& parcel) {
  if (parcels_.empty() || !parcels_[0].has_value()) {
    return false;
  }

  Entry& head = *parcels_[0];
  parcel = std::move(head.parcel);

  ABSL_ASSERT(num_parcels_ > 0);
  --num_parcels_;
  ++base_sequence_number_;

  // Make sure the next queued entry has up-to-date accounting, if present.
  if (parcels_.size() > 1 && parcels_[1]) {
    Entry& next = *parcels_[1];
    next.span_start = head.span_start;
    next.span_end = head.span_end;
    next.num_parcels_in_span = head.num_parcels_in_span - 1;
    next.num_bytes_in_span = head.num_bytes_in_span - parcel.data_view().size();
  }

  parcels_[0].reset();
  parcels_ = parcels_.subspan(1);

  // If there's definitely no more populated parcel data, take this opporunity
  // to realign `parcels_` to the front of `storage_` to reduce future
  // allocations.
  if (num_parcels_ == 0) {
    parcels_ = ParcelView(storage_.data(), parcels_.size());
  }

  return true;
}

Parcel& IncomingParcelQueue::NextParcel() {
  ABSL_ASSERT(HasNextParcel());
  return parcels_[0]->parcel;
}

void IncomingParcelQueue::Reallocate(SequenceNumber sequence_length) {
  size_t parcels_offset = parcels_.data() - storage_.data();
  size_t new_parcels_size = sequence_length - base_sequence_number_;
  if (parcels_offset + new_parcels_size < storage_.size()) {
    // Fast path: just extend the view into storage.
    parcels_ = ParcelView(storage_.data() + parcels_offset, new_parcels_size);
    return;
  }

  // We need to reallocate storage. Re-align `parcels_` with the front of the
  // buffer, and leave some extra room when allocating.
  if (parcels_offset > 0) {
    std::move(parcels_.begin(), parcels_.end(), storage_.begin());
  }
  storage_.resize(new_parcels_size * 2);
  parcels_ = ParcelView(storage_.data(), new_parcels_size);
}

void IncomingParcelQueue::PlaceNewEntry(size_t index, Parcel& parcel) {
  ABSL_ASSERT(index < parcels_.size());
  ABSL_ASSERT(!parcels_[index].has_value());

  const SequenceNumber sequence_number = parcel.sequence_number();
  parcels_[index].emplace();
  Entry& entry = *parcels_[index];
  entry.num_parcels_in_span = 1;
  entry.num_bytes_in_span = parcel.data_view().size();
  entry.parcel = std::move(parcel);

  if (index == 0 || !parcels_[index - 1]) {
    entry.span_start = sequence_number;
  } else {
    Entry& left = *parcels_[index - 1];
    entry.span_start = left.span_start;
    entry.num_parcels_in_span += left.num_parcels_in_span;
    entry.num_bytes_in_span += left.num_bytes_in_span;
  }

  if (index == parcels_.size() - 1 || !parcels_[index + 1]) {
    entry.span_end = sequence_number;
  } else {
    Entry& right = *parcels_[index + 1];
    entry.span_end = right.span_end;
    entry.num_parcels_in_span += right.num_parcels_in_span;
    entry.num_bytes_in_span += right.num_bytes_in_span;
  }

  Entry* start;
  if (entry.span_start <= base_sequence_number_) {
    start = &parcels_[0].value();
  } else {
    start = &parcels_[entry.span_start - base_sequence_number_].value();
  }

  ABSL_ASSERT(entry.span_end >= base_sequence_number_);
  size_t end_index = entry.span_end - base_sequence_number_;
  ABSL_ASSERT(end_index < parcels_.size());
  Entry* end = &parcels_[end_index].value();

  start->span_end = entry.span_end;
  start->num_parcels_in_span = entry.num_parcels_in_span;
  start->num_bytes_in_span = entry.num_bytes_in_span;

  end->span_start = entry.span_start;
  end->num_parcels_in_span = entry.num_parcels_in_span;
  end->num_bytes_in_span = entry.num_bytes_in_span;

  ++num_parcels_;
}

IncomingParcelQueue::Entry::Entry() = default;

IncomingParcelQueue::Entry::Entry(Entry&&) = default;

IncomingParcelQueue::Entry& IncomingParcelQueue::Entry::operator=(Entry&&) =
    default;

IncomingParcelQueue::Entry::~Entry() = default;

}  // namespace core
}  // namespace ipcz
