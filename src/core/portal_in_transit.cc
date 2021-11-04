// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/portal_in_transit.h"

namespace ipcz {
namespace core {

PortalInTransit::PortalInTransit() = default;

PortalInTransit::PortalInTransit(PortalInTransit&&) = default;

PortalInTransit& PortalInTransit::operator=(PortalInTransit&&) = default;

PortalInTransit::~PortalInTransit() = default;

}  // namespace core
}  // namespace ipcz
