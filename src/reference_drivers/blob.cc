// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "reference_drivers/blob.h"

#include <iterator>

#include "reference_drivers/handle_util.h"
#include "util/ref_counted.h"

namespace ipcz::reference_drivers {

Blob::RefCountedFlag::RefCountedFlag() = default;

Blob::RefCountedFlag::~RefCountedFlag() = default;

Blob::Blob(std::string_view message, absl::Span<OSHandle> handles)
    : Object(kBlob),
      message_(message),
      handles_(std::move_iterator(handles.begin()),
               std::move_iterator(handles.end())) {}

Blob::~Blob() = default;

IpczResult Blob::Close() {
  destruction_flag_->set(true);
  return IPCZ_RESULT_OK;
}

// static
IpczDriverHandle Blob::Create(std::string_view message,
                              absl::Span<OSHandle> handles) {
  return AcquireHandle(MakeRefCounted<Blob>(message, handles));
}

// static
IpczDriverHandle Blob::AcquireHandle(Ref<Blob> blob) {
  return ToDriverHandle(blob.release());
}

// static
Blob* Blob::FromHandle(IpczDriverHandle handle) {
  Object* object = Object::FromHandle(handle);
  if (!object || object->type() != kBlob) {
    return nullptr;
  }

  return static_cast<Blob*>(object);
}

// static
Ref<Blob> Blob::ReleaseFromHandle(IpczDriverHandle handle) {
  return Ref<Blob>(FromHandle(handle));
}

}  // namespace ipcz::reference_drivers
