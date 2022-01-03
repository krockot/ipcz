// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node_link_address.h"

#include "core/buffer_id.h"
#include "ipcz/ipcz.h"

namespace ipcz {
namespace core {

NodeLinkAddress::NodeLinkAddress() = default;

NodeLinkAddress::NodeLinkAddress(const NodeLinkAddress&) = default;

NodeLinkAddress& NodeLinkAddress::operator=(const NodeLinkAddress&) = default;

NodeLinkAddress::NodeLinkAddress(BufferId buffer_id, uint64_t offset)
    : buffer_id_(buffer_id), offset_(offset) {}

NodeLinkAddress::~NodeLinkAddress() = default;

}  // namespace core
}  // namespace ipcz
