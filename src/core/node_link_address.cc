// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node_link_address.h"

#include <sstream>

#include "core/buffer_id.h"
#include "ipcz/ipcz.h"

namespace ipcz {
namespace core {

NodeLinkAddress::NodeLinkAddress(const NodeLinkAddress&) = default;

NodeLinkAddress& NodeLinkAddress::operator=(const NodeLinkAddress&) = default;

std::string NodeLinkAddress::ToString() const {
  if (is_null()) {
    return "(null)";
  }

  std::stringstream ss;
  ss << "<" << buffer_id_ << ":" << offset_ << ">";
  return ss.str();
}

}  // namespace core
}  // namespace ipcz
