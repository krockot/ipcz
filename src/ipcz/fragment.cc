// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/fragment.h"

#include <cstdint>
#include <sstream>

namespace ipcz {

Fragment::Fragment(const FragmentDescriptor& descriptor, void* address)
    : descriptor_(descriptor), address_(address) {}

Fragment::Fragment(const Fragment&) = default;

Fragment& Fragment::operator=(const Fragment&) = default;

std::string Fragment::ToString() const {
  std::stringstream ss;
  ss << descriptor_.ToString() << ":" << address_;
  return ss.str();
}

}  // namespace ipcz
