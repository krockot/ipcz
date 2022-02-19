// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_UTIL_FUNCTION_H_
#define IPCZ_SRC_UTIL_FUNCTION_H_

#include <cstddef>
#include <memory>
#include <utility>

namespace ipcz {

namespace internal {

template <typename R, typename... Args>
class FunctionBase {
 public:
  virtual ~FunctionBase() = default;
  virtual R Run(Args... args) const = 0;
};

template <typename F, typename R, typename... Args>
class FunctionImpl : public FunctionBase<R, Args...> {
 public:
  FunctionImpl() : func_(nullptr) {}
  FunctionImpl(F func) : func_(std::move(func)) {}
  FunctionImpl(FunctionImpl&&) = default;
  FunctionImpl& operator=(FunctionImpl&&) = default;
  ~FunctionImpl() override = default;

  R Run(Args... args) const override {
    return func_(std::forward<Args>(args)...);
  }

 private:
  mutable F func_;
};

}  // namespace internal

template <typename F>
class Function;

// Helper class to partially erase movable lambda types and reduce them to
// something distinguished only by call signature.
template <typename R, typename... Args>
class Function<R(Args...)> {
 public:
  Function() = default;
  Function(std::nullptr_t) {}

  Function(R (*f)(Args...))
      : Function([f](Args... args) { return f(std::forward<Args>(args)...); }) {
  }

  template <typename F>
  Function(F func)
      : func_(std::make_unique<internal::FunctionImpl<F, R, Args...>>(
            std::move(func))) {}

  Function(Function&&) = default;
  Function& operator=(Function&&) = default;
  ~Function() = default;

  explicit operator bool() const { return func_ != nullptr; }

  R operator()(Args... args) const {
    return func_->Run(std::forward<Args>(args)...);
  }

 private:
  std::unique_ptr<internal::FunctionBase<R, Args...>> func_;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_UTIL_FUNCTION_H_
