// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_OUTGOING_PARCEL_QUEUE_H_
#define IPCZ_SRC_CORE_OUTGOING_PARCEL_QUEUE_H_

#include <cstddef>
#include <forward_list>

#include "core/parcel.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

// OutgoingParcelQueue retains a FIFO container of Parcels where ordering is
// determined strictly by arrival time. This queue is suitable for queueing
// parcels to be sent later from the same location, and for accumulating parcels
// from another source to be forwarded.
//
// NOTE: Unlike IncomingParcelQueue, Parcels here are NOT strictly ordered by
// sequence number.
class OutgoingParcelQueue {
 public:
  OutgoingParcelQueue();
  OutgoingParcelQueue(OutgoingParcelQueue&& other);
  OutgoingParcelQueue& operator=(OutgoingParcelQueue&& other);
  ~OutgoingParcelQueue();

  bool empty() const { return parcels_.empty(); }
  size_t size() const { return size_; }
  size_t data_size() const { return data_size_; }

  void clear();
  Parcel& front();
  Parcel& back();
  Parcel pop();
  void push(Parcel parcel);

  std::forward_list<Parcel> TakeParcels();

 private:
  std::forward_list<Parcel> parcels_;
  size_t size_ = 0;
  size_t data_size_ = 0;
  std::forward_list<Parcel>::iterator last_parcel_{parcels_.before_begin()};
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_OUTGOING_PARCEL_QUEUE_H_
