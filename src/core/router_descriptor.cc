// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/router_descriptor.h"

namespace ipcz {
namespace core {

RouterDescriptor::RouterDescriptor() = default;

RouterDescriptor::RouterDescriptor(const RouterDescriptor&) = default;

RouterDescriptor& RouterDescriptor::operator=(const RouterDescriptor&) =
    default;

RouterDescriptor::~RouterDescriptor() = default;

}  // namespace core
}  // namespace ipcz
