// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PARCEL_H_
#define IPCZ_SRC_CORE_PARCEL_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/sequence_number.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class Portal;

// Represents a parcel queued within a portal, either for inbound retrieval or
// outgoing transfer. Each parcel contains an optional link to the next Parcel
// in its queue.
//
// TODO: These will need sequence numbers assigned by the sender so they can be
// ordered by the receiver. This is necessary because some parcels will have to
// be relayed through the broker and some may be sent from different nodes (due
// to portal relocation), so we can't guarantee that they'll arrive in the same
// order they were sent.
class Parcel {
 public:
  using PortalVector = absl::InlinedVector<mem::Ref<Portal>, 4>;

  Parcel();
  explicit Parcel(SequenceNumber sequence_number);
  Parcel(Parcel&& other);
  Parcel& operator=(Parcel&& other);
  ~Parcel();

  void set_sequence_number(SequenceNumber n) { sequence_number_ = n; }
  SequenceNumber sequence_number() const { return sequence_number_; }

  void SetData(std::vector<uint8_t> data);
  void SetPortals(PortalVector portals);
  void SetOSHandles(std::vector<os::Handle> os_handles);

  void ResizeData(size_t size);

  const absl::Span<uint8_t>& data_view() const { return data_view_; }

  absl::Span<mem::Ref<Portal>> portals_view() {
    return absl::MakeSpan(portals_);
  }

  absl::Span<const mem::Ref<Portal>> portals_view() const {
    return absl::MakeSpan(portals_);
  }

  absl::Span<os::Handle> os_handles_view() {
    return absl::MakeSpan(os_handles_);
  }

  absl::Span<const os::Handle> os_handles_view() const {
    return absl::MakeSpan(os_handles_);
  }

  void Consume(IpczHandle* portals, IpczOSHandle* os_handles);
  void ConsumePartial(size_t num_bytes_consumed,
                      IpczHandle* portals,
                      IpczOSHandle* os_handles);

  PortalVector TakePortals();

  // Produces a log-friendly description of the Parcel, useful for various
  // debugging log messages.
  std::string Describe() const;

 private:
  void ConsumePortalsAndHandles(IpczHandle* portals, IpczOSHandle* os_handles);

  SequenceNumber sequence_number_ = 0;
  std::vector<uint8_t> data_;
  PortalVector portals_;
  std::vector<os::Handle> os_handles_;

  // A subspan of `data_` tracking the unconsumed bytes in a Parcel which has
  // been partially consumed by one or more two-phase Get() operations.
  absl::Span<uint8_t> data_view_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PARCEL_H_
