// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_API_API_TEST_H_
#define IPCZ_SRC_API_API_TEST_H_

#include <cstddef>
#include <string>
#include <vector>

#include "ipcz/ipcz.h"
#include "os/handle.h"
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

  Parcel Get(IpczHandle portal) {
    uint32_t num_bytes = 0;
    uint32_t num_portals = 0;
    uint32_t num_os_handles = 0;
    IpczResult result =
        ipcz.Get(portal, IPCZ_NO_FLAGS, nullptr, nullptr, &num_bytes, nullptr,
                 &num_portals, nullptr, &num_os_handles);
    if (result == IPCZ_RESULT_OK) {
      return {};
    }

    Parcel parcel;
    std::vector<char> data(num_bytes);
    std::vector<IpczOSHandle> ipcz_os_handles(num_os_handles);
    parcel.portals.resize(num_portals);
    ABSL_ASSERT(result == IPCZ_RESULT_RESOURCE_EXHAUSTED);
    ABSL_ASSERT(ipcz.Get(portal, IPCZ_NO_FLAGS, nullptr, data.data(),
                         &num_bytes, parcel.portals.data(), &num_portals,
                         ipcz_os_handles.data(),
                         &num_os_handles) == IPCZ_RESULT_OK);
    parcel.message = {data.begin(), data.end()};
    parcel.os_handles.resize(num_os_handles);
    for (size_t i = 0; i < num_os_handles; ++i) {
      parcel.os_handles[i] = os::Handle::FromIpczOSHandle(ipcz_os_handles[i]);
    }
    return parcel;
  }

 private:
  IpczHandle node_;
};

}  // namespace test
}  // namespace ipcz

#endif  // IPCZ_SRC_API_API_TEST_H_
