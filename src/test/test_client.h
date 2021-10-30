// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_TEST_TEST_CLIENT_H_
#define IPCZ_SRC_TEST_TEST_CLIENT_H_

#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "os/channel.h"
#include "os/process.h"

namespace ipcz {
namespace test {

// Launches and maintains an os::Channel connected to a child process which runs
// a named entry point. Usage is as follows:
//
// TEST_F(MyTest, DoStuff) {
//   // Launch a new child process running the "MyClient" entry point defined
//   // below.
//   TestClient client("MyClient");
//
//   // Do some communication over the channel.
//   client.channel()->Send(...);
//
//   // Wait for the client process to terminate.
//   client.Wait();
// }
//
// TEST_CLIENT(MyClient, c) {
//   // Listen for a message on the channel before terminating. `c` is an
//   // os::Channel connected to the `client.channel()` used above.
//   WaitForMessage(&c);
// }
class TestClient {
 public:
  // Launches a new child process which will run the named entry point upon
  // startup.
  explicit TestClient(const char* entry_point);
  ~TestClient();

  static void SetInClientProcess(bool in_client_process);
  static bool InClientProcess();

  const os::Process& process() const { return process_; }
  os::Channel& channel() { return channel_; }

  // Waits for the child process to terminate and returns its exit code.
  int Wait();

 private:
  os::Process process_;
  os::Channel channel_;
};

// Defines a new entry point for test child processes. Tests can use this macro
// to define a new entry point and then use the TestClient helper class above to
// to launch a new process which executes that entry point.
//
// The entry point will be defined as a method on a new class derived from
// `fixture`, allowing for a base fixture class to provide common client utility
// methods for several distinct test client definitions. To define a client
// entry point without a specific base fixture, use TEST_CLIENT().
#define TEST_CLIENT_F(fixture, name, channel)                                 \
  class IpczTestClient_##name : public fixture {                              \
   public:                                                                    \
    IpczTestClient_##name() = default;                                        \
    ~IpczTestClient_##name() = default;                                       \
    static void Run(uint64_t channel_handle);                                 \
    void TestBody() override {}                                               \
                                                                              \
   private:                                                                   \
    void DoRun(::ipcz::os::Channel channel);                                  \
  };                                                                          \
  void IpczTestClient_##name::Run(uint64_t channel_handle) {                  \
    ::ipcz::test::TestClient::SetInClientProcess(true);                       \
    IpczTestClient_##name client;                                             \
    client.DoRun(                                                             \
        ::ipcz::test::internal::TestClientSupport::RecoverClientChannel(      \
            channel_handle));                                                 \
  }                                                                           \
  ::ipcz::test::internal::ClientEntryPointRegistration<IpczTestClient_##name> \
      g_register_IpczTestClient_##name{"" #name};                             \
  void IpczTestClient_##name::DoRun(::ipcz::os::Channel channel)

// Like TEST_CLIENT_F() but does not specify a custom fixture for the client.
#define TEST_CLIENT(name, channel) TEST_CLIENT_F(::testing::Test, name, channel)

namespace internal {

class TestClientSupport {
 public:
  static void SetCurrentProgram(const char* path);
  static void RegisterEntryPoint(const char* name,
                                 std::function<void(uint64_t)> entry_point);
  static void RunEntryPoint(const std::string& name, uint64_t channel_handle);
  static os::Channel RecoverClientChannel(uint64_t channel_handle);
};

template <typename T>
class ClientEntryPointRegistration {
 public:
  explicit ClientEntryPointRegistration(const char* name) {
    TestClientSupport::RegisterEntryPoint(name, &T::Run);
  }
};

}  // namespace internal

}  // namespace test
}  // namespace ipcz

#endif  // IPCZ_SRC_TEST_TEST_CLIENT_H_
