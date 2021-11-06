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
// light of this, portals place incoming parcels into an IncomingParcelQueue.
//
// Based on the assumption that temporary sequence gaps are common but tend to
// be small, this retains at least enough sparse linear storage to hold every
// parcel between the last popped sequence number (exclusive) and the highest
// received sequence number so far (inclusive). As parcels are consumed from the
// queue this storage may be efficiently compacted to reduce waste.
class IncomingParcelQueue {
 public:
  IncomingParcelQueue();
  IncomingParcelQueue(SequenceNumber current_sequence_number);
  IncomingParcelQueue(IncomingParcelQueue&& other);
  IncomingParcelQueue& operator=(IncomingParcelQueue&& other);
  ~IncomingParcelQueue();

  // The next sequence number in queue. This starts at the constructor's
  // `current_sequence_number` and increments any time a parcel is successfully
  // popped from the queue.
  SequenceNumber current_sequence_number() const {
    return base_sequence_number_;
  }

  // The final length of the peer's parcel sequence which we're receiving. Null
  // if the peer isn't closed yet.
  const absl::optional<SequenceNumber>& peer_sequence_length() const {
    return peer_sequence_length_;
  }

  // Returns the number of parcels currently ready for popping at the front of
  // the queue. This is the number of *contiguous* sequenced parcels available
  // starting from `current_sequence_number()`.
  size_t GetNumAvailableParcels() const;

  // Returns the sum of the number of data bytes in each currently available
  // parcel (the same parcels counted by GetNumAvailableParcels()).
  size_t GetNumAvailableBytes() const;

  // Sets the known final length of the incoming parcel sequence. This is the
  // sequence number of the peer side's last outgoing parcel, plus 1; or it's 0
  // if the peer side was closed without sending any parcels.
  //
  // May fail and return false if the queue already has parcels with a sequence
  // number higher than or equal to `length`, or if it had already set a
  // peer sequence length before this call. Either case is likely the result of
  // a misbehaving node and should be treated as a validation failure.
  bool SetPeerSequenceLength(SequenceNumber length);

  // Indicates whether this queue is still waiting for someone to push more
  // parcels. This is always true if SetPeerSequenceLength(n) has not been
  // called. Once that has been called, this remains true only until ALL
  // parcels up to and including sequence number `n` have been pushed. From that
  // point onward, this will always return false.
  bool IsExpectingMoreParcels() const;

  // The next sequence number expected by this queue, if any.
  absl::optional<SequenceNumber> GetNextExpectedSequenceNumber() const;

  // Indicates whether the next parcel (in sequence order) is available to pop.
  bool HasNextParcel() const;

  // Indicates whether this incoming queue is "dead," meaning it will no longer
  // accept new incoming messages and there will never be another parcel to pop.
  bool IsDead() const;

  // This may fail if `n` falls below the minimum or maximum (when applicable)
  // expected sequence number for parcels in this queue.
  bool Push(Parcel parcel);

  // Pops the next (in sequence order) parcel off the queue if available,
  // populating `parcel` with its contents and returning true on success. On
  // failure `parcel` is untouched and this returns false.
  bool Pop(Parcel& parcel);

  // Gets a reference to the next parcel. This reference is NOT stable across
  // ANY non-const methods on this object.
  Parcel& NextParcel();

 private:
  void Reallocate(SequenceNumber sequence_length);
  void PlaceNewEntry(size_t index, Parcel& parcel);

  struct Entry {
    Entry();
    Entry(Entry&& other);
    Entry& operator=(Entry&&);
    ~Entry();

    Parcel parcel;

    // NOTE: The fields below are maintained during Push and Pop operations and
    // are used to support efficient implementation of GetNumAvailableParcels()
    // and GetNumAvailableBytes(). This warrants some clarification.
    //
    // Conceptually we treat the active range of parcel entries as a series of
    // contiguous spans:
    //
    //     `parcels_`: [2][ ][4][5][6][ ][8][9]
    //
    // For example, above we can designate three contiguous spans: parcel 2
    // stands alone at the front of the queue, parcels 4-6 form a second span,
    // and then parcels 8-9 form the third. Parcels 3 and 7 are absent.
    //
    // We're interested in knowing how many parcels (and their total size in
    // bytes) are available right now, which means we want to answer the
    // question: how long is the span starting at element 0? In this case since
    // parcel 2 stands alone at the front of the queue, the answer is 1. There's
    // 1 parcel available right now.
    //
    // If we pop parcel 2 off the queue, it then becomes:
    //
    //     `parcels_`: [ ][4][5][6][ ][8][9]
    //
    // The head of the queue is pointing at the empty slot for parcel 3, and
    // because no span starts in element 0 there are now 0 parcels available for
    // popping.
    //
    // Finally if we then push parcel 3, the queue looks like this:
    //
    //     `parcels_`: [3][4][5][6][ ][8][9]
    //
    // and now there are 4 parcels available to pop. Element 0 begins the span
    // of parcels 3, 4, 5, and 6.
    //
    // To answer the question efficiently though, each entry records some
    // metadata about the span in which it resides. This information is not kept
    // up-to-date for all entries, but we maintain the invariant that the first
    // and last element of each distinct span has accurate metadata; and as a
    // consequence if any span starts at element 0, then we know element 0's
    // metadata accurately answers our general questions about the queue.
    //
    // When a parcel with sequence number N is inserted into the queue, it can
    // be classified in one of four ways:
    //
    //    (1) it stands alone with no parcel at N-1 or N+1
    //    (2) it follows a parcel at N-1, but N+1 is empty
    //    (3) it precedes a parcel at N+1, but N-1 is empty
    //    (4) it falls between a parcel at N-1 and a parcel at N+1.
    //
    // In case (1) we record in the entry that its span starts and ends at
    // parcel N; we also record the length of the span (1), along with its data
    // size (which is just this parcel's own data size). This entry now has
    // trivially correct metadata about its containing span, of which it is both
    // the head and tail.
    //
    // In case (2), parcel N is now the tail of a pre-existing span. Because
    // tail elements are always up-to-date, we simply copy and augment the data
    // from the old tail (parcel N-1) into the new tail (parcel N). From this
    // data we also know where the head of the span is, so we can update it with
    // the same new metadata.
    //
    // Case (3) is similar to case (2). Parcel N is now the head of a
    // pre-existing span, so we copy and augment the already up-to-date N+1
    // entry's metadata (the old head) into our new entry as well as the span's
    // tail entry.
    //
    // Case (4) is joining two pre-existing spans. In this case parcel N fetches
    // the span's start from parcel N-1 (the tail of the span to the left), and
    // the span's end from parcel N+1 (the head of the span to the right); and
    // it sums their parcel and byte counts with its own. This new combined
    // metadata is copied into both the head of the left span and the tail of
    // the right span, and with parcel N populated this now constitutes a single
    // combined span with accurate metadata in its head and tail entries.
    //
    // Finally, the only other operation that matters for this accounting is
    // Pop(). All Pop() needs to do though is update the metadata in the new
    // head-of-queue (if present) after popping the previous head. This update
    // is trivially derived from the popped entry's own metadata.
    size_t num_parcels_in_span = 0;
    size_t num_bytes_in_span = 0;
    SequenceNumber span_start = 0;
    SequenceNumber span_end = 0;
  };

  using ParcelStorage = absl::InlinedVector<absl::optional<Entry>, 4>;
  using ParcelView = absl::Span<absl::optional<Entry>>;

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
  ParcelView parcels_{storage_.data(), 0};

  // The sequence number which corresponds to `parcels_` index 0 when `parcels_`
  // is non-empty.
  SequenceNumber base_sequence_number_ = 0;

  // The number of elements in `parcels_` which are actually occupied.
  size_t num_parcels_ = 0;

  // The known final length of the sequence of parcels sent to us. Set only if
  // known, and known only once the peer is closed.
  absl::optional<SequenceNumber> peer_sequence_length_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_INCOMING_PARCEL_QUEUE_H_
