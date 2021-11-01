// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PARCEL_H_
#define IPCZ_SRC_CORE_PARCEL_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/portal.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

// Represents a parcel queued within a portal, either for inbound retrieval or
// outgoing transfer. Each parcel contains an optional link to the next Parcel
// in its queue.
class Parcel {
 public:
  Parcel();
  Parcel(Parcel&& other);
  Parcel& operator=(Parcel&& other);
  ~Parcel();

  void SetData(std::vector<uint8_t> data);
  void SetPortals(std::vector<PortalInTransit> portals);
  void SetOSHandles(std::vector<os::Handle> os_handles);

  void ResizeData(size_t size);

  const absl::Span<uint8_t>& data_view() const { return data_view_; }

  absl::Span<PortalInTransit> portals_view() {
    return absl::MakeSpan(portals_);
  }

  absl::Span<os::Handle> os_handles_view() {
    return absl::MakeSpan(os_handles_);
  }

  void Consume(IpczHandle* portals, IpczOSHandle* os_handles);
  void ConsumePartial(size_t num_bytes_consumed,
                      IpczHandle* portals,
                      IpczOSHandle* os_handles);

  std::vector<PortalInTransit> TakePortals();

 private:
  void ConsumePortalsAndHandles(IpczHandle* portals, IpczOSHandle* os_handles);

  std::vector<uint8_t> data_;
  std::vector<PortalInTransit> portals_;
  std::vector<os::Handle> os_handles_;

  // A subspan of `data_` tracking the unconsumed bytes in a Parcel which has
  // been partially consumed by one or more two-phase Get() operations.
  absl::Span<uint8_t> data_view_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PARCEL_H_
