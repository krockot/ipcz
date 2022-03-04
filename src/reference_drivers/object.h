// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_REFERENCE_DRIVERS_OBJECT_H_
#define IPCZ_SRC_REFERENCE_DRIVERS_OBJECT_H_

#include <cstdint>

#include "ipcz/ipcz.h"
#include "util/handle_util.h"
#include "util/ref_counted.h"

namespace ipcz::reference_drivers {

// Base class for all driver-managed objects used by both reference drivers.
class Object : public RefCounted {
 public:
  enum Type : uint32_t {
    // Types needed to support ipcz operation.
    kTransport,
    kMemory,
    kMapping,
    kOSHandle,

    // Custom types used only by these reference drivers.
    kBlob,
    kUnserializableGarbage,
  };

  explicit Object(Type type);

  Type type() const { return type_; }

  static Object* FromHandle(IpczDriverHandle handle) {
    return ToPtr<Object>(handle);
  }

  static IpczDriverHandle ReleaseAsHandle(Ref<Object> object) {
    return static_cast<IpczDriverHandle>(
        reinterpret_cast<uintptr_t>(object.release()));
  }

  static Ref<Object> ReleaseFromHandle(IpczDriverHandle handle) {
    return Ref<Object>(RefCounted::kAdoptExistingRef, FromHandle(handle));
  }

  template <typename T>
  static Ref<T> ReleaseFromHandleAs(IpczDriverHandle handle) {
    return Ref<T>(static_cast<T*>(ReleaseFromHandle(handle).release()));
  }

  template <typename T>
  T& As() {
    return static_cast<T&>(*this);
  }

  template <typename T>
  Ref<T> ReleaseAs() {
    return Ref<T>(RefCounted::kAdoptExistingRef, static_cast<T*>(this));
  }

  virtual IpczResult Close();

 protected:
  ~Object() override;

 private:
  const Type type_;
};

}  // namespace ipcz::reference_drivers

#endif  // IPCZ_SRC_REFERENCE_DRIVERS_OBJECT_H_
