// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/portal_backend_state.h"

namespace ipcz {
namespace core {

PortalBackendState::PortalBackendState() = default;

PortalBackendState::PortalBackendState(PortalBackendState&&) = default;

PortalBackendState& PortalBackendState::operator=(PortalBackendState&&) =
    default;

PortalBackendState::~PortalBackendState() = default;

}  // namespace core
}  // namespace ipcz
