// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_TEST_TEST_BASE_H_
#define IPCZ_SRC_TEST_TEST_BASE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ipcz/ipcz.h"
#include "os/handle.h"
#include "os/process.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/types/span.h"

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
    std::vector<os::Handle> os_handles;
  };

  TestBase();
  ~TestBase() override;

  IpczAPI ipcz;

  template <size_t N>
  void ClosePortals(const IpczHandle (&handles)[N]) {
    for (IpczHandle handle : handles) {
      ASSERT_EQ(IPCZ_RESULT_OK,
                ipcz.ClosePortal(handle, IPCZ_NO_FLAGS, nullptr));
    }
  }

  template <size_t N>
  void DestroyNodes(const IpczHandle (&handles)[N]) {
    for (IpczHandle handle : handles) {
      ASSERT_EQ(IPCZ_RESULT_OK,
                ipcz.DestroyNode(handle, IPCZ_NO_FLAGS, nullptr));
    }
  }

  IpczHandle ConnectNode(IpczHandle node,
                         IpczDriverHandle driver_transport,
                         const os::Process& process,
                         IpczCreateNodeFlags flags);
  IpczHandle ConnectToBroker(IpczHandle node,
                             IpczDriverHandle driver_transport);
  IpczHandle ConnectToNonBroker(IpczHandle node,
                                IpczDriverHandle driver_transport,
                                const os::Process& process);

  void OpenPortals(IpczHandle node, IpczHandle* a, IpczHandle* b);

  void Put(IpczHandle portal,
           const std::string& str,
           absl::Span<IpczHandle> portals = {},
           absl::Span<os::Handle> os_handles = {});
  IpczResult MaybeGet(IpczHandle portal, Parcel& parcel);
  IpczResult WaitToGet(IpczHandle portal, Parcel& parcel);
  Parcel Get(IpczHandle portal);

  void VerifyEndToEnd(IpczHandle a, IpczHandle b, size_t num_iterations);
  void WaitForProxyDecay(IpczHandle a, IpczHandle b);
};

}  // namespace test
}  // namespace ipcz

#endif  // IPCZ_SRC_TEST_TEST_BASE_H_
