// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_API_API_TEST_H_
#define IPCZ_SRC_API_API_TEST_H_

#include <cstddef>
#include <string>

#include "ipcz/ipcz.h"
#include "os/handle.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace test {

class APITest : public testing::Test {
 public:
  APITest();
  ~APITest() override;

  IpczHandle node() const { return node_; }

  IpczAPI ipcz;

  // For convenience, every API test starts with connected portals q and p.
  IpczHandle q;
  IpczHandle p;

  void OpenPortals(IpczHandle* a, IpczHandle* b) {
    ASSERT_EQ(IPCZ_RESULT_OK,
              ipcz.OpenPortals(node_, IPCZ_NO_FLAGS, nullptr, a, b));
  }

  template <size_t N>
  void ClosePortals(const IpczHandle (&handles)[N]) {
    for (IpczHandle handle : handles) {
      ASSERT_EQ(IPCZ_RESULT_OK,
                ipcz.ClosePortal(handle, IPCZ_NO_FLAGS, nullptr));
    }
  }

  void Put(IpczHandle portal,
           const std::string& str,
           absl::Span<IpczHandle> portals = {},
           absl::Span<os::Handle> os_handles = {}) {
    std::vector<IpczOSHandle> handles(os_handles.size());
    for (size_t i = 0; i < os_handles.size(); ++i) {
      handles[i].size = sizeof(handles[i]);
      os::Handle::ToIpczOSHandle(std::move(os_handles[i]), &handles[i]);
    }

    ASSERT_EQ(IPCZ_RESULT_OK,
              ipcz.Put(portal, str.data(), static_cast<uint32_t>(str.length()),
                       portals.data(), static_cast<uint32_t>(portals.size()),
                       handles.data(), static_cast<uint32_t>(handles.size()),
                       IPCZ_NO_FLAGS, nullptr));
  }

 private:
  IpczHandle node_;
};

}  // namespace test
}  // namespace ipcz

#endif  // IPCZ_SRC_API_API_TEST_H_
