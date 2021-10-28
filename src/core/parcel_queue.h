// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PARCEL_QUEUE_H_
#define IPCZ_SRC_CORE_PARCEL_QUEUE_H_

#include <cstddef>
#include <forward_list>

#include "core/parcel.h"

namespace ipcz {
namespace core {

class ParcelQueue {
 public:
  ParcelQueue();
  ParcelQueue(ParcelQueue&& other);
  ParcelQueue& operator=(ParcelQueue&& other);
  ~ParcelQueue();

  bool empty() const { return parcels_.empty(); }
  size_t size() const { return size_; }

  void clear();
  Parcel& front();
  Parcel pop();
  void push(Parcel parcel);

  std::forward_list<Parcel> TakeParcels();

 private:
  std::forward_list<Parcel> parcels_;
  size_t size_ = 0;
  std::forward_list<Parcel>::iterator last_parcel_{parcels_.before_begin()};
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PARCEL_QUEUE_H_
