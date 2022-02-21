// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_TEST_TEST_BASE_H_
#define IPCZ_SRC_TEST_TEST_BASE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ipcz/ipcz.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/os_handle.h"
#include "util/os_process.h"

namespace ipcz {
namespace test {

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
    std::vector<IpczHandle> portals;
    std::vector<OSHandle> os_handles;
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
                         const OSProcess& process,
                         IpczCreateNodeFlags flags);
  IpczHandle ConnectToBroker(IpczHandle node,
                             IpczDriverHandle driver_transport);
  IpczHandle ConnectToNonBroker(IpczHandle node,
                                IpczDriverHandle driver_transport,
                                const OSProcess& process);

  void OpenPortals(IpczHandle node, IpczHandle* a, IpczHandle* b);

  void Put(IpczHandle portal,
           const std::string& str,
           absl::Span<IpczHandle> handles = {},
           absl::Span<OSHandle> os_handles = {});
  IpczResult MaybeGet(IpczHandle portal, Parcel& parcel);
  IpczResult WaitToGet(IpczHandle portal, Parcel& parcel);
  Parcel Get(IpczHandle portal);
  bool DiscardNextParcel(IpczHandle portal);

  void VerifyEndToEnd(IpczHandle a, IpczHandle b);
  bool PortalsAreLocalPeers(IpczHandle a, IpczHandle b);
  void LogPortalRoute(IpczHandle a);
  static size_t GetNumRouters();
  static void DumpAllRouters();
};

}  // namespace test
}  // namespace ipcz

#endif  // IPCZ_SRC_TEST_TEST_BASE_H_
