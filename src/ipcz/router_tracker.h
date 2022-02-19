// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_ROUTER_TRACKER_H_
#define IPCZ_SRC_IPCZ_ROUTER_TRACKER_H_

#include <atomic>
#include <cstddef>
#include <set>

#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {

class Router;

// Helper class used to track and dump information about existing Router
// instances in the process. Primarily used for debugging. In non-debug builds
// this only keeps an atomic count of the total number of Router instances.
class RouterTracker {
 public:
  RouterTracker();
  ~RouterTracker();

  static void Track(Router* router);
  static void Untrack(Router* router);
  static size_t GetNumRouters();
  static void DumpRouters();

 private:
#ifdef NDEBUG
  std::atomic<size_t> num_routers_{0};
#else
  absl::Mutex mutex_;
  std::set<Router*> routers_ ABSL_GUARDED_BY(mutex_);
#endif
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_ROUTER_TRACKER_H_
