// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_TEST_TEST_BASE_H_
#define IPCZ_SRC_TEST_TEST_BASE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "ipcz/ipcz.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/synchronization/notification.h"
#include "third_party/abseil-cpp/absl/time/time.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz::test {

class TestBase : public testing::Test {
 public:
  struct Parcel {
    Parcel();
    Parcel(Parcel&&);
    Parcel& operator=(Parcel&&);
    Parcel(const Parcel&) = delete;
    Parcel& operator=(const Parcel&) = delete;
    ~Parcel();
    std::string message;
    std::vector<IpczHandle> handles;
  };

  // Helper to set a timeout and run some diagnostics in case of test hangs.
  class HangTimeout {
   public:
    HangTimeout(absl::Duration timeout, std::function<void()> handler);
    ~HangTimeout();

   private:
    absl::Notification notification_;
    std::thread thread_;
  };

  TestBase();
  ~TestBase() override;

  IpczAPI ipcz;

  template <size_t N>
  void CloseHandles(const IpczHandle (&handles)[N]) {
    for (IpczHandle handle : handles) {
      ASSERT_EQ(IPCZ_RESULT_OK, ipcz.Close(handle, IPCZ_NO_FLAGS, nullptr));
    }
  }

  template <size_t N>
  void ClosePortals(const IpczHandle (&handles)[N]) {
    CloseHandles(handles);
  }

  template <size_t N>
  void DestroyNodes(const IpczHandle (&handles)[N]) {
    CloseHandles(handles);
  }

  IpczHandle ConnectNode(IpczHandle node,
                         IpczDriverHandle driver_transport,
                         IpczCreateNodeFlags flags);
  IpczHandle ConnectToBroker(IpczHandle node,
                             IpczDriverHandle driver_transport);
  IpczHandle ConnectToNonBroker(IpczHandle node,
                                IpczDriverHandle driver_transport);

  void OpenPortals(IpczHandle node, IpczHandle* a, IpczHandle* b);

  void Put(IpczHandle portal,
           const std::string& str,
           absl::Span<IpczHandle> handles = {});
  IpczResult MaybeGet(IpczHandle portal, Parcel& parcel);
  IpczResult WaitForIncomingParcel(IpczHandle portal);
  IpczResult WaitToGet(IpczHandle portal, Parcel& parcel);
  Parcel Get(IpczHandle portal);
  bool DiscardNextParcel(IpczHandle portal);

  using Handler = std::function<void(const IpczTrapEvent& e)>;
  IpczResult Trap(IpczHandle portal,
                  const IpczTrapConditions& conditions,
                  Handler handler,
                  IpczTrapConditionFlags* satisfied_condition_flags = nullptr,
                  IpczPortalStatus* status = nullptr);

  void VerifyEndToEnd(IpczHandle a, IpczHandle b);
  bool PortalsAreLocalPeers(IpczHandle a, IpczHandle b);
  void LogPortalRoute(IpczHandle a);
  static size_t GetNumRouters();
  static void DumpAllRouters();
  static void DiagnoseNode(IpczHandle node);

 private:
  static void OnTrapEvent(const IpczTrapEvent* event);
};

}  // namespace ipcz::test

#endif  // IPCZ_SRC_TEST_TEST_BASE_H_
