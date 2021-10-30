// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/test_client.h"

#include <cstdio>
#include <tuple>

#include "build/build_config.h"

#if defined(OS_POSIX)
#include <errno.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ipcz {
namespace test {

namespace {

const char* g_current_program;
bool g_in_client_process = false;

using EntryPointMap = std::map<std::string, std::function<void(uint64_t)>>;

EntryPointMap& GetEntryPoints() {
  static EntryPointMap* entry_points = new EntryPointMap();
  return *entry_points;
}

}  // namespace

namespace internal {

// static
void TestClientSupport::SetCurrentProgram(const char* path) {
  g_current_program = path;
}

// static
void TestClientSupport::RegisterEntryPoint(
    const char* name,
    std::function<void(uint64_t)> entry_point) {
  auto result =
      GetEntryPoints().insert(std::make_pair(std::string(name), entry_point));
  ABSL_ASSERT(result.second);
}

// static
void TestClientSupport::RunEntryPoint(const std::string& name,
                                      uint64_t channel_handle) {
  auto it = GetEntryPoints().find(name);
  ABSL_ASSERT(it != GetEntryPoints().end());
  it->second(channel_handle);
}

// static
os::Channel TestClientSupport::RecoverClientChannel(uint64_t channel_handle) {
#if defined(OS_POSIX)
  return os::Channel(os::Handle(static_cast<int>(channel_handle)));
#else
#error "Need to implement this for the current platform."
#endif
}

}  // namespace internal

// static
void TestClient::SetInClientProcess(bool in_client_process) {
  g_in_client_process = in_client_process;
}

// static
bool TestClient::InClientProcess() {
  return g_in_client_process;
}

TestClient::TestClient(const char* entry_point) {
  os::Channel client_channel;
  std::tie(channel_, client_channel) = os::Channel::CreateChannelPair();

  ABSL_ASSERT(channel_.is_valid());
  ABSL_ASSERT(client_channel.is_valid());

#if defined(OS_POSIX)
  pid_t pid = fork();
  ABSL_ASSERT(pid >= 0);

  if (pid == 0) {
    // Child.
    char handle_string[32];
    int client_fd = client_channel.TakeHandle().ReleaseFD();
    int result = snprintf(handle_string, 32, "%d", client_fd);
    ABSL_ASSERT(result < 32);

    // Close any open descriptors except stdio and our channel.
    rlimit limit;
    result = getrlimit(RLIMIT_NOFILE, &limit);
    ABSL_ASSERT(result == 0);
    for (unsigned int fd = STDERR_FILENO + 1; fd < limit.rlim_cur; ++fd) {
      if (static_cast<int>(fd) != client_fd) {
        close(fd);
      }
    }

    result = execl(g_current_program, g_current_program, "--run_test_client",
                   entry_point, "--client_channel_handle", &handle_string[0],
                   nullptr);
    perror("failed to launch test client: ");
    ABSL_ASSERT(result == 0);
    return;
  }

  process_ = os::Process(pid);
#else
#error "Need to implement this for the current platform."
#endif
}

TestClient::~TestClient() {
  channel_.Reset();
  if (process_.is_valid()) {
    Wait();
  }
}

int TestClient::Wait() {
#if defined(OS_POSIX)
  ABSL_ASSERT(process_.is_valid());

  int status;
  pid_t result = waitpid(process_.handle(), &status, 0);
  ABSL_ASSERT(result == process_.handle());
  process_.reset();

  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }

  return -1;
#else
#error "Need to implement this for the current platform."
#endif
}

}  // namespace test
}  // namespace ipcz
