// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/portal_descriptor.h"

namespace ipcz {
namespace core {

PortalDescriptor::PortalDescriptor() = default;

PortalDescriptor::PortalDescriptor(const PortalDescriptor&) = default;

PortalDescriptor& PortalDescriptor::operator=(const PortalDescriptor&) =
    default;

PortalDescriptor::~PortalDescriptor() = default;

}  // namespace core
}  // namespace ipcz
