// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_INCOMING_PARCEL_QUEUE_H_
#define IPCZ_SRC_CORE_INCOMING_PARCEL_QUEUE_H_

#include <cstddef>
#include <vector>

#include "core/parcel.h"
#include "core/sequence_number.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace ipcz {
namespace core {

// IncomingParcelQueue retains a queue of Parcel objects strictly ordered by
// sequence number.
//
// All parcels are assigned a sequence number by the portal that produced them,
// and this number is increased with every parcel sent. Because portals may move
// across nodes and because some parcels may be relayed through the broker on
// some platforms, it is prohibitively difficult to ensure that parcels always
// arrive at their destination in the same order in which they were sent. In
// light of this, portals place incoming parcel into an IncomingParcelQueue.
//
// Based on the assumption that temporary sequence gaps are common but tend to
// be small, this retains at least enough sparse linear storage to hold every
// parcel between the last popped sequence number (exclusive) and the highest
// received sequence number so far (inclusive). As parcels are consumed from the
// queue this storage may be efficiently compacted to reduce waste.
class IncomingParcelQueue {
 public:
  IncomingParcelQueue();
  IncomingParcelQueue(SequenceNumber first_sequence_number);
  IncomingParcelQueue(IncomingParcelQueue&& other);
  IncomingParcelQueue& operator=(IncomingParcelQueue&& other);
  ~IncomingParcelQueue();

  // Gets the size of the incoming message queue. This is NOT necessarily the
  // number of parcels actually present in the queue, but is instead the
  // difference between the highest known or expected sequence number so far,
  // and the the last consumed sequence number from the same sequence.
  size_t GetSize() const;

  // Sets the sequence number of the last parcel this queue will receive, if
  // known. May fail and return false if the queue already has parcels with a
  // sequence number higher than `n` or already had set a highest expected
  // sequence number. Either case is likely the result of a misbehaving node.
  bool SetHighestExpectedSequenceNumber(SequenceNumber n);

  // Indicates whether this queue is still waiting for someone to push more
  // parcels. This is always true if SetHighestExpectedSequenceNumber(n) has not
  // been called. Once that has been called, this remains true only until ALL
  // parcels up to and including sequence number `n` have been pushed. From that
  // point onward, this will always return false.
  bool IsExpectingMoreParcels() const;

  // Indicates whether the next parcel (in sequence order) is available to pop.
  bool HasNextParcel() const;

  // This may fail if `n` falls below the minimum or maximum (when applicable)
  // expected sequence number for parcels in this queue.
  bool Push(Parcel& parcel);

  // Pops the next (in sequence order) parcel off the queue if available,
  // populating `parcel` with its contents and returning true on success. On
  // failure `parcel` is untouched and this returns false.
  bool Pop(Parcel& parcel);

  // Gets a reference to the next parcel. This reference is NOT stable across
  // ANY non-const methods on this object.
  Parcel& NextParcel();

 private:
  using ParcelStorage = absl::InlinedVector<absl::optional<Parcel>, 4>;
  using ParcelSpan = absl::Span<absl::optional<Parcel>>;

  void Reallocate(SequenceNumber max_sequence_number);

  // This is a sparse vector of incoming parcels indexed by a relative sequence
  // number.
  //
  // It's "sparse" because the queue may receive parcels 42 and 47 before it
  // receives parcels 43-46. We tolerate the temporarily wasted storage in such
  // cases.
  //
  // TODO: use a proper sparse vector implementation? would like to guard
  // against egregious abuse like setting a very high expected sequence # and
  // forcing OOM, but without sorting parcels.
  ParcelStorage storage_;

  // A view into `storage_` whose first element corresponds to the parcel with
  // sequence number `base_sequence_number_`. As parcels are popped, the view
  // moves forward in `storage_`. When convenient, we may reallocate `storage_`
  // and realign this view.
  ParcelSpan parcels_{storage_.data(), 0};

  // The sequence number which corresponds to `parcels_` index 0 when `parcels_`
  // is non-empty.
  SequenceNumber base_sequence_number_ = 0;

  // The number of elements in `parcels_` which are actually occupied.
  size_t num_parcels_ = 0;

  // The last sequence number we expect to see, if set.
  absl::optional<SequenceNumber> highest_expected_sequence_number_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_INCOMING_PARCEL_QUEUE_H_
