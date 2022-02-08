// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/fragment_descriptor.h"

#include <sstream>

#include "core/buffer_id.h"
#include "ipcz/ipcz.h"

namespace ipcz {
namespace core {

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

}  // namespace core
}  // namespace ipcz
