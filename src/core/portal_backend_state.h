// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_BACKEND_STATE_H_
#define IPCZ_SRC_CORE_PORTAL_BACKEND_STATE_H_

#include "core/parcel.h"
#include "core/parcel_queue.h"
#include "core/trap.h"
#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace ipcz {
namespace core {

// State which is generally common to every type of backend.
struct PortalBackendState {
  PortalBackendState();
  PortalBackendState(PortalBackendState&&);
  PortalBackendState& operator=(PortalBackendState&&);
  ~PortalBackendState();

  // Indicates whether the portal itself (not its peer) has been closed.
  bool closed = false;

  // The current status of the portal as reflected by various ipcz APIs.
  IpczPortalStatus status = {sizeof(status)};

  // An in-progress parcel being built by a two-phase Put operation.
  // TODO: This should be a more robust interface so e.g. RoutedPortalBackend
  // can build a parcel directly in shared memory.
  absl::optional<Parcel> pending_parcel;

  // Queue of outgoing parcels that can't be delivered to their destination yet.
  ParcelQueue outgoing_parcels;

  // Queue of incoming messages which are available for retrieval by the
  // application.
  ParcelQueue incoming_parcels;

  // Indicates whether a two-phase Get operation is currently in progress. While
  // this is `true`, the application retains a pointer into memory owned by
  // the head of `incoming_parcels`, so that Parcel must remain in a stable
  // state.
  bool in_two_phase_get = false;

  // The set of traps currently attached to the portal.
  TrapSet traps;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_BACKEND_STATE_H_
