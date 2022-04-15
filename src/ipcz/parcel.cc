// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/parcel.h"

#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>

#include "ipcz/box.h"
#include "ipcz/driver_object.h"
#include "ipcz/fragment_allocator.h"
#include "ipcz/node_link_memory.h"
#include "ipcz/portal.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {

Parcel::Parcel() = default;

Parcel::Parcel(SequenceNumber sequence_number)
    : sequence_number_(sequence_number) {}

Parcel::Parcel(Parcel&& other)
    : sequence_number_(other.sequence_number_),
      data_fragment_(other.data_fragment_),
      inlined_data_(std::move(other.inlined_data_)),
      data_fragment_memory_(std::move(other.data_fragment_memory_)),
      objects_(std::move(other.objects_)),
      data_view_(other.data_view_),
      objects_view_(other.objects_view_) {
  other.data_fragment_ = {};
  other.data_view_ = {};
  other.objects_view_ = {};
}

Parcel& Parcel::operator=(Parcel&& other) {
  sequence_number_ = other.sequence_number_;
  data_fragment_ = other.data_fragment_;
  inlined_data_ = std::move(other.inlined_data_);
  data_fragment_memory_ = std::move(other.data_fragment_memory_);
  objects_ = std::move(other.objects_);
  data_view_ = other.data_view_;
  objects_view_ = other.objects_view_;
  other.data_fragment_ = {};
  other.data_view_ = {};
  other.objects_view_ = {};
  return *this;
}

Parcel::~Parcel() {
  for (Ref<APIObject>& object : objects_) {
    if (object) {
      object->Close();
    }
  }

  if (!data_fragment_.is_null()) {
    ABSL_ASSERT(data_fragment_memory_);
    data_fragment_memory_->fragment_allocator().Free(data_fragment_);
  }
}

void Parcel::SetDataFragment(Ref<NodeLinkMemory> memory,
                             const Fragment& fragment) {
  data_fragment_ = fragment;
  data_fragment_memory_ = std::move(memory);
  if (fragment.is_resolved()) {
    data_view_ = fragment.mutable_bytes();
  } else {
    data_view_ = {};
  }
}

void Parcel::SetInlinedData(std::vector<uint8_t> data) {
  inlined_data_ = std::move(data);
  data_view_ = absl::MakeSpan(inlined_data_);
}

void Parcel::SetObjects(std::vector<Ref<APIObject>> objects) {
  objects_ = std::move(objects);
  objects_view_ = absl::MakeSpan(objects_);
}

void Parcel::ReleaseDataFragment() {
  ABSL_ASSERT(!data_fragment_.is_null());
  data_fragment_ = {};
  data_fragment_memory_.reset();
  data_view_ = {};
}

bool Parcel::ResolveDataFragment() {
  ABSL_ASSERT(!data_fragment_.is_null());
  if (!data_fragment_.is_pending()) {
    return true;
  }

  data_fragment_ =
      data_fragment_memory_->GetFragment(data_fragment_.descriptor());
  if (!data_fragment_.is_resolved()) {
    return false;
  }

  data_view_ = data_fragment_.mutable_bytes();
  return true;
}

void Parcel::Consume(size_t num_bytes, absl::Span<IpczHandle> out_handles) {
  auto data = data_view();
  auto objects = objects_view();
  ABSL_ASSERT(num_bytes <= data.size());
  ABSL_ASSERT(out_handles.size() <= objects.size());
  for (size_t i = 0; i < out_handles.size(); ++i) {
    out_handles[i] = APIObject::ReleaseAsHandle(std::move(objects[i]));
  }

  data_view_.remove_prefix(num_bytes);
  objects_view_.remove_prefix(out_handles.size());
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
  } else if (data_fragment_.is_pending()) {
    ss << "data pending";
  } else {
    ss << "no data";
  }
  if (!objects_view().empty()) {
    ss << ", " << num_objects() << " handles";
  }
  ss << ")";
  return ss.str();
}

bool Parcel::CanTransmitOn(const DriverTransport& transport) {
  for (auto object : objects_) {
    if (auto* box = Box::FromObject(object.get())) {
      if (!box->object().CanTransmitOn(transport)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace ipcz
