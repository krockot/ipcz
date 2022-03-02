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

  bool empty() const {
    return data_offset_ == data_.size() && object_offset_ == objects_.size() &&
           os_handle_offset_ == os_handles_.size();
  }

  void SetData(std::vector<uint8_t> data);
  void SetObjects(ObjectVector objects);
  void SetOSHandles(std::vector<OSHandle> os_handles);

  void ResizeData(size_t size);

  absl::Span<uint8_t> data_view() {
    return absl::MakeSpan(data_).subspan(data_offset_);
  }
  absl::Span<const uint8_t> data_view() const {
    return absl::MakeSpan(data_).subspan(data_offset_);
  }
  size_t data_size() const { return data_view().size(); }

  absl::Span<Ref<APIObject>> objects_view() {
    return absl::MakeSpan(objects_).subspan(object_offset_);
  }
  absl::Span<const Ref<APIObject>> objects_view() const {
    return absl::MakeSpan(objects_).subspan(object_offset_);
  }
  size_t num_objects() const { return objects_view().size(); }

  absl::Span<OSHandle> os_handles_view() {
    return absl::MakeSpan(os_handles_).subspan(os_handle_offset_);
  }
  absl::Span<const OSHandle> os_handles_view() const {
    return absl::MakeSpan(os_handles_).subspan(os_handle_offset_);
  }
  size_t num_os_handles() const { return os_handles_view().size(); }

  void Consume(size_t num_bytes,
               absl::Span<IpczHandle> out_handles,
               absl::Span<IpczOSHandle> out_os_handles);

  // Produces a log-friendly description of the Parcel, useful for various
  // debugging log messages.
  std::string Describe() const;

 private:
  SequenceNumber sequence_number_ = 0;
  std::vector<uint8_t> data_;
  ObjectVector objects_;
  std::vector<OSHandle> os_handles_;

  // Base indices into the above storage vectors, tracking the first unconsumed
  // element in each.
  size_t data_offset_ = 0;
  size_t object_offset_ = 0;
  size_t os_handle_offset_ = 0;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_PARCEL_H_
