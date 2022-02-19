// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/fragment_descriptor.h"

#include <sstream>

#include "ipcz/buffer_id.h"
#include "ipcz/ipcz.h"

namespace ipcz {

FragmentDescriptor::FragmentDescriptor(const FragmentDescriptor&) = default;

FragmentDescriptor& FragmentDescriptor::operator=(const FragmentDescriptor&) =
    default;

std::string FragmentDescriptor::ToString() const {
  if (is_null()) {
    return "(null)";
  }

  std::stringstream ss;
  ss << "<" << buffer_id_ << ":" << offset_ << ">";
  return ss.str();
}

}  // namespace ipcz
