// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PARCEL_QUEUE_H_
#define IPCZ_SRC_CORE_PARCEL_QUEUE_H_

#include <cstddef>
#include <vector>

#include "core/parcel.h"
#include "core/sequence_number.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace ipcz {
namespace core {

// ParcelQueue retains a queue of Parcel objects strictly ordered by sequence
// number.
//
// All parcels are assigned a sequence number by the portal that produced them,
// and this number is increased with every parcel sent. Because portals may move
// across nodes and because some parcels may be relayed through the broker on
// some platforms, it is prohibitively difficult to ensure that parcels always
// arrive at their destination in the same order in which they were sent. In
// light of this, portals place incoming parcels into a ParcelQueue.
//
// Based on the assumption that temporary sequence gaps are common but tend to
// be small, this retains at least enough linear storage to hold every parcel
// between the last popped sequence number (exclusive) and the highest received
// or known sequence number so far (inclusive). This storage may only be
// sparsely populated at times, but as parcels are consumed from the queue,
// storage is compacted to reduce waste.
class ParcelQueue {
 public:
  ParcelQueue();
  ParcelQueue(SequenceNumber initial_sequence_number);
  ParcelQueue(ParcelQueue&& other);
  ParcelQueue& operator=(ParcelQueue&& other);
  ~ParcelQueue();

  // The next sequence number that is or will be available from the queue. This
  // starts at the constructor's `initial_sequence_number` and increments any
  // time a parcel is successfully popped from the queue.
  SequenceNumber current_sequence_number() const {
    return base_sequence_number_;
  }

  // The final length of the parcel sequence which we're receiving. Null if the
  // final length is not yet known. If this is N, then the last parcel that can
  // be pushed to or popped from the ParcelQueue is parcel N-1.
  const absl::optional<SequenceNumber>& final_sequence_length() const {
    return final_sequence_length_;
  }

  // Returns the number of parcels currently ready for popping at the front of
  // the queue. This is the number of *contiguously* sequenced parcels available
  // starting from `current_sequence_number()`.
  size_t GetNumAvailableParcels() const;

  // Returns the sum of the number of data bytes in each currently available
  // parcel; that is, the sum of byte sizes of the parcels counted by
  // GetNumAvailableParcels().
  size_t GetNumAvailableBytes() const;

  // Returns the length of the Parcel sequence seen so far by this queue. This
  // is essentially `current_sequence_number()` plus `GetNumAvailableParcels()`.
  // For example if `current_sequence_number()` is 5 and
  // `GetNumAvailableParcels()` is 3, then parcels 5, 6, and 7 are available
  // for retrieval and the total length of the sequence so far is  8; so this
  // method would return 8.
  SequenceNumber GetCurrentSequenceLength() const;

  // Sets the known final length of the incoming parcel sequence. This is the
  // sequence number of the last parcel that can be pushed, plus 1; or 0 if no
  // parcels can be pushed.
  //
  // May fail and return false if the queue already has parcels with a sequence
  // number greater than or equal to `length`, or if a the final sequence length
  // had already been set prior to this call.
  bool SetFinalSequenceLength(SequenceNumber length);

  // Indicates whether this queue is still waiting to have more parcels pushed.
  // This is always true if the final sequence length has not been set yet. Once
  // the final sequence length is set, this remains true only until all parcels
  // between the initial sequence number (inclusive) and the final sequence
  // length (exclusive) have been pushed into (and optionally popped from) the
  // queue.
  bool IsExpectingMoreParcels() const;

  // Indicates whether the next parcel (in sequence order) is available to pop.
  bool HasNextParcel() const;

  // Indicates that there are no parcels in this queue, not even parcels beyond
  // the current sequence number that are merely inaccessible.
  bool IsEmpty() const;

  // Indicates whether this queue is "dead," meaning it will no longer accept
  // new parcels and there are no more parcels left pop. This occurs iff the
  // final sequence length is known, and all parcels from the initial sequence
  // number up to the final sequence length have been pushed into and then
  // popped from this queue.
  bool IsDead() const;

  // Resets this ParcelQueue to start at the initial SequenceNumber `n`. Must
  // only be called on an empty ParcelQueue (IsEmpty() == true) and only when
  // the caller can be sure they don't want to push any parcels with a
  // SequenceNumber below `n`.
  void ResetInitialSequenceNumber(SequenceNumber n);

  // This may fail if `n` falls below the minimum or maximum (when applicable)
  // expected sequence number for parcels in this queue.
  bool Push(Parcel parcel);

  // Pops the next (in sequence order) parcel off the queue if available,
  // populating `parcel` with its contents and returning true on success. On
  // failure `parcel` is untouched and this returns false.
  bool Pop(Parcel& parcel);

  // Gets a reference to the next parcel. This reference is NOT stable across
  // ANY non-const ParcelQueue methods.
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
    // Pop(). All Pop() needs to do though is derive new metadata for the new
    // head-of-queue's span (if present) after popping. This metadata will
    // update both the new head-of-queue as well as its span's tail.
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
  absl::optional<SequenceNumber> final_sequence_length_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PARCEL_QUEUE_H_
