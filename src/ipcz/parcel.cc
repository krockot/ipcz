// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/parcel.h"

#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>

#include "ipcz/portal.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/handle_util.h"

namespace ipcz {

Parcel::Parcel() = default;

Parcel::Parcel(SequenceNumber sequence_number)
    : sequence_number_(sequence_number) {}

Parcel::Parcel(Parcel&& other) = default;

Parcel& Parcel::operator=(Parcel&& other) = default;

Parcel::~Parcel() {
  for (Ref<APIObject>& object : objects_) {
    if (object) {
      object->Close();
    }
  }
}

void Parcel::SetData(std::vector<uint8_t> data) {
  data_ = std::move(data);
  data_view_ = absl::MakeSpan(data_);
}

void Parcel::SetObjects(ObjectVector objects) {
  objects_ = std::move(objects);
}

void Parcel::SetOSHandles(std::vector<OSHandle> os_handles) {
  os_handles_ = std::move(os_handles);
}

void Parcel::ResizeData(size_t size) {
  data_.resize(size);
  data_view_ = absl::MakeSpan(data_);
}

void Parcel::Consume(IpczHandle* handles, IpczOSHandle* os_handles) {
  ConsumeHandles(handles, os_handles);
  data_view_ = {};
}

void Parcel::ConsumePartial(size_t num_bytes_consumed,
                            IpczHandle* handles,
                            IpczOSHandle* os_handles) {
  data_view_ = data_view_.subspan(num_bytes_consumed);
  ConsumeHandles(handles, os_handles);
}

void Parcel::ConsumeHandles(IpczHandle* handles, IpczOSHandle* os_handles) {
  for (size_t i = 0; i < objects_.size(); ++i) {
    handles[i] = ToHandle(objects_[i].release());
  }
  for (size_t i = 0; i < os_handles_.size(); ++i) {
    OSHandle::ToIpczOSHandle(std::move(os_handles_[i]), &os_handles[i]);
  }
  objects_.clear();
  os_handles_.clear();
}

std::string Parcel::Describe() const {
  std::stringstream ss;
  ss << "parcel " << sequence_number() << " (";
  if (!data_view().empty()) {
    // Cheesy heuristic: if the first character is an ASCII letter or number,
    // assume the parcel data is human-readable and print a few characters.
    if (std::isalnum(data_view()[0])) {
      const absl::Span<const uint8_t> preview = data_view().subspan(0, 8);
      ss << "\"" << std::string(preview.begin(), preview.end());
      if (preview.size() < data_size()) {
        ss << "...\", " << data_size() << " bytes";
      } else {
        ss << '"';
      }
    }
  } else {
    ss << "no data";
  }
  if (!objects_view().empty()) {
    ss << ", " << num_objects() << " handles";
  }
  if (!os_handles_view().empty()) {
    ss << ", " << num_os_handles() << " OS handles";
  }
  ss << ")";
  return ss.str();
}

}  // namespace ipcz
