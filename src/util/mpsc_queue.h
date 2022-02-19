// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_UTIL_MPSC_QUEUE_H_
#define IPCZ_SRC_UTIL_MPSC_QUEUE_H_

#include <cstddef>
#include <cstdint>

#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {

namespace internal {

// Generic implementation base for MpscQueue<T>.
class MpscQueueBase {
 public:
  MpscQueueBase();
  MpscQueueBase(absl::Span<uint8_t> region, size_t element_size);
  MpscQueueBase(const MpscQueueBase&);
  MpscQueueBase& operator=(const MpscQueueBase&);
  ~MpscQueueBase();

  static size_t ComputeSpaceRequiredFor(size_t element_size,
                                        size_t num_elements);

  // One-time initialization of the queue's underlying region. Many MpscQueue
  // instances may operate over the same region, but only one must initialize
  // the region first.
  void InitializeRegion();

  // Pops a single element off of the queue. Returns true on success or false if
  // there was no element available to pop.
  //
  // Note that while many MpscQueue instances may operate on the same underlying
  // region, only one may call consume elements through Pop(). Consumer state is
  // NOT held in the underlying memory region but is local to this MpscQueue
  // instance.
  bool Pop();

 protected:
  // Pushes data into the queue. The size of `data` must be exactly the same
  // `element_size` with which this queue was constructed (and with which the
  // underlying region was initialized). Returns true on success or false if
  // there was not enough space in the queue.
  bool PushBytes(absl::Span<const uint8_t> bytes);

  // Peeks at the head of the queue, returning a pointer to the head element if
  // there is one. Otherwise null is returned.
  void* PeekBytes();

 private:
  struct Data;

  Data& data() { return *reinterpret_cast<Data*>(region_.data()); }

  absl::Span<uint8_t> region_;
  size_t element_size_ = 0;
  size_t num_cells_ = 0;
  size_t max_index_ = 0;
  size_t head_ = 0;
};

}  // namespace internal

// MpscQueue is a multiple-producer, single-consumer, bounded, lock-free queue
// structure suitable for use by any number of concurrent producers and a single
// consumer. The underlying data type T must be trivially copyable.
//
// MpscQueue itself should live in a process's private memory, but the
// underlying memory region structure may live in untrusted shared memory.
template <typename T>
class MpscQueue : public internal::MpscQueueBase {
 public:
  MpscQueue() = default;
  explicit MpscQueue(absl::Span<uint8_t> region)
      : MpscQueueBase(region, sizeof(T)) {}
  MpscQueue(const MpscQueue&) = default;
  MpscQueue& operator=(const MpscQueue&) = default;
  ~MpscQueue() = default;

  static size_t ComputeSpaceRequiredFor(size_t num_elements) {
    return MpscQueueBase::ComputeSpaceRequiredFor(sizeof(T), num_elements);
  }

  // Pushes `value` onto the queue if there's room, and returns true on success.
  // Returns false on failure, implying there was no available capacity for a
  // new element.
  bool Push(const T& value) {
    return PushBytes(absl::MakeSpan(reinterpret_cast<const uint8_t*>(&value),
                                    sizeof(value)));
  }

  // Peeks at the head of the queue, returning a pointer to the frontmost value
  // if one is present. Returns null otherwise.
  T* Peek() { return static_cast<T*>(PeekBytes()); }
};

}  // namespace ipcz

#endif  // IPCZ_SRC_UTIL_MPSC_QUEUE_H_
