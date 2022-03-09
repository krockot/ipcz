// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_API_OBJECT_H_
#define IPCZ_SRC_IPCZ_API_OBJECT_H_

#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "util/handle_util.h"
#include "util/ref_counted.h"

namespace ipcz {

class Portal;

// Base class for any object which can be referenced by an IpczHandle.
//
// Subclasses must have a corresponding entry in the ObjectType enum below, and
// they must define a static constexpr object_type() method indicating which
// type they identify as.
class APIObject : public RefCounted {
 public:
  enum ObjectType {
    kNode,
    kPortal,
    kBox,
  };

  explicit APIObject(ObjectType type);

  ObjectType object_type() const { return type_; }

  template <typename T>
  static T* Get(IpczHandle handle) {
    APIObject* object = ToPtr<APIObject>(handle);
    if (object && object->type_ == T::object_type()) {
      return static_cast<T*>(object);
    }
    return nullptr;
  }

  template <typename T>
  T* GetAs() {
    if (type_ == T::object_type()) {
      return static_cast<T*>(this);
    }
    return nullptr;
  }

  template <typename T>
  T& As() {
    ABSL_ASSERT(type_ == T::object_type());
    return static_cast<T&>(*this);
  }

  // Relinquishes ownership of this object such that it's no longer managed by
  // an IpczHandle.
  static Ref<APIObject> ReleaseHandle(IpczHandle handle) {
    return Ref<APIObject>(RefCounted::kAdoptExistingRef,
                          ToPtr<APIObject>(handle));
  }

  // Same as above but with type checking.
  template <typename T>
  static Ref<T> ReleaseHandleAs(IpczHandle handle) {
    APIObject* object = ToPtr<APIObject>(handle);
    if (!object || object->type_ != T::object_type()) {
      return nullptr;
    }
    return Ref<T>(RefCounted::kAdoptExistingRef, static_cast<T*>(object));
  }

  // Closes this underlying object, ceasing its operations and freeing its
  // resources ASAP.
  virtual IpczResult Close() = 0;

  // Indicates whether it's possible to send this object from `sender`. By
  // default the answer is NO.
  virtual bool CanSendFrom(Portal& sender);

 protected:
  ~APIObject() override = default;

 private:
  const ObjectType type_;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_API_OBJECT_H_
