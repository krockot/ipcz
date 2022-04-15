// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_PARCEL_H_
#define IPCZ_SRC_IPCZ_PARCEL_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ipcz/api_object.h"
#include "ipcz/fragment.h"
#include "ipcz/ipcz.h"
#include "ipcz/sequence_number.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/ref_counted.h"

namespace ipcz {

class DriverTransport;
class NodeLinkMemory;

// Represents a parcel queued within a portal, either for inbound retrieval or
// outgoing transfer.
class Parcel {
 public:
  Parcel();
  explicit Parcel(SequenceNumber sequence_number);
  Parcel(Parcel&& other);
  Parcel& operator=(Parcel&& other);
  ~Parcel();

  void set_sequence_number(SequenceNumber n) { sequence_number_ = n; }
  SequenceNumber sequence_number() const { return sequence_number_; }

  bool empty() const { return data_view_.empty() && objects_view_.empty(); }

  void SetDataFragment(Ref<NodeLinkMemory> memory, const Fragment& fragment);
  void SetInlinedData(std::vector<uint8_t> data);
  void SetObjects(std::vector<Ref<APIObject>> objects);

  absl::Span<uint8_t> data_view() { return data_view_; }
  absl::Span<const uint8_t> data_view() const { return data_view_; }

  size_t data_size() const { return data_view().size(); }

  const Fragment& data_fragment() const { return data_fragment_; }
  const Ref<NodeLinkMemory>& data_fragment_memory() const {
    return data_fragment_memory_;
  }

  absl::Span<Ref<APIObject>> objects_view() { return objects_view_; }
  absl::Span<const Ref<APIObject>> objects_view() const {
    return objects_view_;
  }
  size_t num_objects() const { return objects_view().size(); }

  // Sets the actual size of this parcel's data. Must be no larger than the
  // existing size of `data_view()`.
  void SetDataSize(size_t num_bytes) {
    data_view_ = data_view_.subspan(0, num_bytes);
  }

  // Relinquishes ownership of this Parcel's data fragment, if applicable. This
  // prevents the fragment from being freed upon Parcel destruction.
  void ReleaseDataFragment();

  // Attempts to resolve the Parcel's data fragment if it was pending.
  bool ResolveDataFragment();

  void Consume(size_t num_bytes, absl::Span<IpczHandle> out_handles);

  // Produces a log-friendly description of the Parcel, useful for various
  // debugging log messages.
  std::string Describe() const;

  // Checks and indicates whether this parcel can be transmitted entirely over
  // `transport`, which depends on whether the driver is able to transmit all of
  // the attached driver objects over that transport.
  bool CanTransmitOn(const DriverTransport& transport);

 private:
  SequenceNumber sequence_number_{0};

  // Only one or the other of these data fields is valid.
  Fragment data_fragment_;
  std::vector<uint8_t> inlined_data_;

  // Iff `data_fragment_` is set, this is the NodeLinkMemory that responsible
  // for its containing buffer.
  Ref<NodeLinkMemory> data_fragment_memory_;

  std::vector<Ref<APIObject>> objects_;

  // Views of unconsumed elements of the parcel's data (either in `inline_data_`
  // or `data_fragment_`) and objects.
  absl::Span<uint8_t> data_view_;
  absl::Span<Ref<APIObject>> objects_view_;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_PARCEL_H_
