// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/mapped_node_link_address.h"

#include <cstdint>
#include <sstream>

namespace ipcz {
namespace core {

MappedNodeLinkAddress::MappedNodeLinkAddress(const NodeLinkAddress& address,
                                             void* mapped_address)
    : address_(address), mapped_address_(mapped_address) {}

MappedNodeLinkAddress::MappedNodeLinkAddress(const MappedNodeLinkAddress&) =
    default;

MappedNodeLinkAddress& MappedNodeLinkAddress::operator=(
    const MappedNodeLinkAddress&) = default;

std::string MappedNodeLinkAddress::ToString() const {
  std::stringstream ss;
  ss << address_.ToString() << ":" << mapped_address_;
  return ss.str();
}

}  // namespace core
}  // namespace ipcz
