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
#include "ipcz/ipcz.h"
#include "ipcz/sequence_number.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/os_handle.h"
#include "util/ref_counted.h"

namespace ipcz {

// Represents a parcel queued within a portal, either for inbound retrieval or
// outgoing transfer.
class Parcel {
 public:
  using ObjectVector = absl::InlinedVector<Ref<APIObject>, 4>;

  Parcel();
  explicit Parcel(SequenceNumber sequence_number);
  Parcel(Parcel&& other);
  Parcel& operator=(Parcel&& other);
  ~Parcel();

  void set_sequence_number(SequenceNumber n) { sequence_number_ = n; }
  SequenceNumber sequence_number() const { return sequence_number_; }

  void SetData(std::vector<uint8_t> data);
  void SetObjects(ObjectVector objects);
  void SetOSHandles(std::vector<OSHandle> os_handles);

  void ResizeData(size_t size);

  const absl::Span<uint8_t>& data_view() const { return data_view_; }
  size_t data_size() const { return data_view_.size(); }

  absl::Span<Ref<APIObject>> objects_view() { return absl::MakeSpan(objects_); }
  absl::Span<const Ref<APIObject>> objects_view() const {
    return absl::MakeSpan(objects_);
  }
  size_t num_objects() const { return objects_.size(); }

  absl::Span<OSHandle> os_handles_view() { return absl::MakeSpan(os_handles_); }
  absl::Span<const OSHandle> os_handles_view() const {
    return absl::MakeSpan(os_handles_);
  }
  size_t num_os_handles() const { return os_handles_.size(); }

  void Consume(IpczHandle* handles, IpczOSHandle* os_handles);
  void ConsumePartial(size_t num_bytes_consumed,
                      IpczHandle* handles,
                      IpczOSHandle* os_handles);

  // Produces a log-friendly description of the Parcel, useful for various
  // debugging log messages.
  std::string Describe() const;

 private:
  void ConsumeHandles(IpczHandle* handles, IpczOSHandle* os_handles);

  SequenceNumber sequence_number_ = 0;
  std::vector<uint8_t> data_;
  ObjectVector objects_;
  std::vector<OSHandle> os_handles_;

  // A subspan of `data_` tracking the unconsumed bytes in a Parcel which has
  // been partially consumed by one or more two-phase Get() operations.
  absl::Span<uint8_t> data_view_;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_PARCEL_H_
