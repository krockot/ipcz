// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_API_API_TEST_H_
#define IPCZ_SRC_API_API_TEST_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ipcz/ipcz.h"
#include "os/channel.h"
#include "os/event.h"
#include "os/handle.h"
#include "test/test_client.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace test {

class APITest : public testing::Test {
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

  APITest();
  ~APITest() override;

  IpczHandle node() const { return node_; }

  IpczAPI ipcz;

  // For convenience, every API test starts with connected portals q and p.
  IpczHandle q;
  IpczHandle p;

  template <size_t N>
  void ClosePortals(const IpczHandle (&handles)[N]) {
    for (IpczHandle handle : handles) {
      ASSERT_EQ(IPCZ_RESULT_OK,
                ipcz.ClosePortal(handle, IPCZ_NO_FLAGS, nullptr));
    }
  }

  void OpenPortals(IpczHandle* a, IpczHandle* b);
  IpczHandle OpenRemotePortal(IpczHandle node, TestClient& client);
  IpczHandle OpenRemotePortal(IpczHandle node,
                              os::Channel& channel,
                              const os::Process& process);
  IpczHandle AcceptRemotePortal(IpczHandle node, os::Channel& channel);
  void Put(IpczHandle portal,
           const std::string& str,
           absl::Span<IpczHandle> portals = {},
           absl::Span<os::Handle> os_handles = {});
  IpczResult MaybeGet(IpczHandle portal, Parcel& parcel);
  IpczResult WaitToGet(IpczHandle portal, Parcel& parcel);
  Parcel Get(IpczHandle portal);

 private:
  IpczHandle node_;
};

}  // namespace test
}  // namespace ipcz

#endif  // IPCZ_SRC_API_API_TEST_H_
