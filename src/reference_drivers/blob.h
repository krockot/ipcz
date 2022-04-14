// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_REFERENCE_DRIVERS_BLOB_H_
#define IPCZ_SRC_REFERENCE_DRIVERS_BLOB_H_

#include <cstdint>
#include <string_view>
#include <vector>

#include "reference_drivers/object.h"
#include "reference_drivers/os_handle.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/ref_counted.h"

namespace ipcz::reference_drivers {

// A driver-managed object which packages an arbitrary collection of string data
// and OS handles. Blobs are serializable by both reference drivers and are used
// to exercise custom driver object boxing in tests.
//
// Note that unlike the transport and memory objects these reference drivers
// define, a blob is not a type of object known to ipcz. Instead it is used to
// demonstrate how drivers can define arbitrary new types of transferrable to
// extend ipcz.
class Blob : public ObjectImpl<Blob, Object::kBlob> {
 public:
  class RefCountedFlag : public RefCounted {
   public:
    RefCountedFlag();

    bool get() const { return flag_; }
    void set(bool flag) { flag_ = flag; }

   private:
    ~RefCountedFlag() override;
    bool flag_ = false;
  };

  Blob(std::string_view message, absl::Span<OSHandle> handles = {});

  // Object:
  IpczResult Close() override;

  std::string& message() { return message_; }
  std::vector<OSHandle>& handles() { return handles_; }
  const Ref<RefCountedFlag>& destruction_flag() const {
    return destruction_flag_;
  }

  static IpczDriverHandle Create(std::string_view message,
                                 absl::Span<OSHandle> handles = {});

  static IpczDriverHandle AcquireHandle(Ref<Blob> blob);

  static Blob* FromHandle(IpczDriverHandle handle);
  static Ref<Blob> ReleaseFromHandle(IpczDriverHandle handle);

 protected:
  ~Blob() override;

 private:
  std::string message_;
  std::vector<OSHandle> handles_;
  const Ref<RefCountedFlag> destruction_flag_{MakeRefCounted<RefCountedFlag>()};
};

}  // namespace ipcz::reference_drivers

#endif  // IPCZ_SRC_REFERENCE_DRIVERS_BLOB_H_
