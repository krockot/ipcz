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
  data_offset_ = 0;
}

void Parcel::SetObjects(ObjectVector objects) {
  objects_ = std::move(objects);
  object_offset_ = 0;
}

void Parcel::ResizeData(size_t size) {
  data_.resize(size);
}

void Parcel::Consume(size_t num_bytes, absl::Span<IpczHandle> out_handles) {
  auto data = data_view();
  auto objects = objects_view();
  ABSL_ASSERT(num_bytes <= data.size());
  ABSL_ASSERT(out_handles.size() <= objects.size());
  for (size_t i = 0; i < out_handles.size(); ++i) {
    out_handles[i] = ToHandle(objects[i].release());
  }

  data_offset_ += num_bytes;
  object_offset_ += out_handles.size();
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
  ss << ")";
  return ss.str();
}

}  // namespace ipcz
