// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/router_tracker.h"

#include "core/router.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {
namespace core {

namespace {

RouterTracker& GetTracker() {
  // Don't worry about running a static destructor on exit.
  static uint8_t storage[sizeof(RouterTracker)];
  static RouterTracker* tracker = new (&storage[0]) RouterTracker();
  return *tracker;
}

}  // namespace

RouterTracker::RouterTracker() = default;

RouterTracker::~RouterTracker() = default;

// static
void RouterTracker::Track(Router* router) {
  RouterTracker& tracker = GetTracker();
#ifdef NDEBUG
  ++num_routers_;
#else
  absl::MutexLock lock(&tracker.mutex_);
  tracker.routers_.insert(router);
#endif
}

// static
void RouterTracker::Untrack(Router* router) {
  RouterTracker& tracker = GetTracker();
#ifdef NDEBUG
  --num_routers_;
#else
  absl::MutexLock lock(&tracker.mutex_);
  tracker.routers_.erase(router);
#endif
}

// static
size_t RouterTracker::GetNumRouters() {
  RouterTracker& tracker = GetTracker();
#ifdef NDEBUG
  return num_routers_;
#else
  absl::MutexLock lock(&tracker.mutex_);
  return tracker.routers_.size();
#endif
}

// static
void RouterTracker::DumpRouters() {
#ifndef NDEBUG
  RouterTracker& tracker = GetTracker();
  absl::MutexLock lock(&tracker.mutex_);
  for (Router* router : tracker.routers_) {
    router->LogDescription();
  }
#endif
}

}  // namespace core
}  // namespace ipcz
