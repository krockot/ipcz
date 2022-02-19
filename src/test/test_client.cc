// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/test_client.h"

#include <cstring>
#include <sstream>
#include <tuple>

#include "build/build_config.h"
#include "drivers/multiprocess_reference_driver.h"
#include "util/function.h"

#if defined(OS_POSIX)
#include <errno.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(OS_WIN)
#include <windows.h>
#endif

namespace ipcz {
namespace test {

namespace {

const char* g_current_program;
bool g_in_client_process = false;

using EntryPointMap = std::map<std::string, Function<void(uint64_t)>>;

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
    Function<void(uint64_t)> entry_point) {
  auto result = GetEntryPoints().insert(
      std::make_pair(std::string(name), std::move(entry_point)));
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
#elif defined(OS_WIN)
  return os::Channel(os::Handle(
      reinterpret_cast<HANDLE>(static_cast<uintptr_t>(channel_handle))));
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
    int client_fd = client_channel.TakeHandle().ReleaseFD();

    // Close any open descriptors except stdio and our channel.
    rlimit limit;
    int result = getrlimit(RLIMIT_NOFILE, &limit);
    ABSL_ASSERT(result == 0);
    for (unsigned int fd = STDERR_FILENO + 1; fd < limit.rlim_cur; ++fd) {
      if (static_cast<int>(fd) != client_fd) {
        close(fd);
      }
    }

    std::stringstream client_arg_stream;
    client_arg_stream << "--run_test_client=" << entry_point;
    std::string client_arg = client_arg_stream.str();

    std::stringstream handle_arg_stream;
    handle_arg_stream << "--client_channel_handle=" << client_fd;
    std::string handle_arg = handle_arg_stream.str();
    result = execl(g_current_program, g_current_program, client_arg.c_str(),
                   handle_arg.c_str(), nullptr);
    perror("failed to launch test client: ");
    ABSL_ASSERT(result == 0);
    return;
  }

  process_ = os::Process(pid);
#elif defined(OS_WIN)
  STARTUPINFOEXW startup_info = {};
  startup_info.StartupInfo.cb = sizeof(startup_info);
  SIZE_T size = 0;
  ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  auto attribute_list = std::make_unique<char[]>(size);
  auto* attrs =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_list.get());
  if (!::InitializeProcThreadAttributeList(attrs, 1, 0, &size)) {
    return;
  }
  startup_info.lpAttributeList = attrs;

  os::Handle handle = client_channel.TakeHandle();
  HANDLE handle_value = handle.handle();
  ::SetHandleInformation(handle.handle(), HANDLE_FLAG_INHERIT,
                         HANDLE_FLAG_INHERIT);
  ::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                              &handle_value, sizeof(HANDLE), nullptr, nullptr);

  startup_info.StartupInfo.dwFlags =
      STARTF_FORCEOFFFEEDBACK | STARTF_USESHOWWINDOW;
  startup_info.StartupInfo.wShowWindow = SW_HIDE;

  std::wstringstream ss;
  ss << ::GetCommandLineW() << " --run_test_client=" << entry_point
     << " --client_channel_handle="
     << reinterpret_cast<uintptr_t>(handle_value);
  std::wstring new_cmd = ss.str();
  std::vector<wchar_t> new_cmd_data(new_cmd.size() + 1);
  memcpy(new_cmd_data.data(), new_cmd.data(), new_cmd.size() * sizeof(wchar_t));

  PROCESS_INFORMATION process_info = {};
  BOOL ok = ::CreateProcess(nullptr, new_cmd_data.data(), nullptr, nullptr,
                            TRUE, 0, nullptr, nullptr,
                            &startup_info.StartupInfo, &process_info);
  ABSL_ASSERT(ok);
  ::DeleteProcThreadAttributeList(attrs);
  process_ = os::Process(process_info.hProcess);
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
#elif defined(OS_WIN)
  if (::WaitForSingleObject(process_.handle(), INFINITE) != WAIT_OBJECT_0) {
    return -1;
  }
  DWORD exit_code;
  if (!::GetExitCodeProcess(process_.handle(), &exit_code)) {
    return -1;
  }

  return static_cast<int>(exit_code);
#else
#error "Need to implement this for the current platform."
#endif
}

}  // namespace test
}  // namespace ipcz
