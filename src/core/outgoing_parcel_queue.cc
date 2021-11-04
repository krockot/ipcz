// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/outgoing_parcel_queue.h"

#include <utility>

#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace core {

OutgoingParcelQueue::OutgoingParcelQueue() = default;

OutgoingParcelQueue::OutgoingParcelQueue(OutgoingParcelQueue&& other)
    : parcels_(std::move(other.parcels_)), size_(other.size_) {
  if (parcels_.empty()) {
    last_parcel_ = parcels_.before_begin();
  } else {
    last_parcel_ = other.last_parcel_;
  }
  other.clear();
}

OutgoingParcelQueue& OutgoingParcelQueue::operator=(
    OutgoingParcelQueue&& other) {
  if (other.last_parcel_ == other.parcels_.before_begin()) {
    last_parcel_ = parcels_.before_begin();
  } else {
    parcels_ = std::move(other.parcels_);
    size_ = other.size_;
    last_parcel_ = other.last_parcel_;
    other.clear();
  }
  return *this;
}

OutgoingParcelQueue::~OutgoingParcelQueue() = default;

void OutgoingParcelQueue::clear() {
  parcels_.clear();
  size_ = 0;
  last_parcel_ = parcels_.before_begin();
}

Parcel& OutgoingParcelQueue::front() {
  ABSL_ASSERT(!empty());
  return parcels_.front();
}

Parcel OutgoingParcelQueue::pop() {
  ABSL_ASSERT(!empty());
  Parcel parcel = std::move(parcels_.front());
  parcels_.pop_front();
  --size_;
  if (parcels_.empty()) {
    last_parcel_ = parcels_.before_begin();
  }
  return parcel;
}

void OutgoingParcelQueue::push(Parcel parcel) {
  last_parcel_ = parcels_.insert_after(last_parcel_, std::move(parcel));
  ++size_;
}

std::forward_list<Parcel> OutgoingParcelQueue::TakeParcels() {
  std::forward_list<Parcel> parcels = std::move(parcels_);
  parcels_.clear();
  last_parcel_ = parcels_.before_begin();
  size_ = 0;
  return parcels;
}

}  // namespace core
}  // namespace ipcz
