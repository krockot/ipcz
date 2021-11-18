// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/parcel.h"

#include <utility>

#include "core/new_impl.h"
#include "util/handle_util.h"

namespace ipcz {
namespace core {

Parcel::Parcel() = default;

Parcel::Parcel(SequenceNumber sequence_number)
    : sequence_number_(sequence_number) {}

Parcel::Parcel(Parcel&& other) = default;

Parcel& Parcel::operator=(Parcel&& other) = default;

Parcel::~Parcel() = default;

void Parcel::SetData(std::vector<uint8_t> data) {
  data_ = std::move(data);
  data_view_ = absl::MakeSpan(data_);
}

void Parcel::SetPortals(PortalVector portals) {
  portals_ = std::move(portals);
}

void Parcel::SetOSHandles(std::vector<os::Handle> os_handles) {
  os_handles_ = std::move(os_handles);
}

void Parcel::ResizeData(size_t size) {
  data_.resize(size);
  data_view_ = absl::MakeSpan(data_);
}

void Parcel::Consume(IpczHandle* portals, IpczOSHandle* os_handles) {
  ConsumePortalsAndHandles(portals, os_handles);
  data_view_ = {};
}

void Parcel::ConsumePartial(size_t num_bytes_consumed,
                            IpczHandle* portals,
                            IpczOSHandle* os_handles) {
  data_view_ = data_view_.subspan(num_bytes_consumed);
  ConsumePortalsAndHandles(portals, os_handles);
}

void Parcel::ConsumePortalsAndHandles(IpczHandle* portals,
                                      IpczOSHandle* os_handles) {
  for (size_t i = 0; i < portals_.size(); ++i) {
    portals[i] = ToHandle(portals_[i].release());
  }
  for (size_t i = 0; i < os_handles_.size(); ++i) {
    os::Handle::ToIpczOSHandle(std::move(os_handles_[i]), &os_handles[i]);
  }
  portals_.clear();
  os_handles_.clear();
}

Parcel::PortalVector Parcel::TakePortals() {
  return std::move(portals_);
}

}  // namespace core
}  // namespace ipcz
