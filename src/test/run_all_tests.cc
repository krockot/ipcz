// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstring>
#include <map>
#include <string>

#include "build/build_config.h"
#include "test/test_client.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/strings/numbers.h"
#include "third_party/abseil-cpp/absl/strings/str_split.h"

#ifndef NDEBUG
#include "util/stack_trace.h"
#endif

#if defined(IPCZ_STANDALONE)
#include "standalone/base/logging.h"
#endif

#if defined(OS_WIN)
#include <windows.h>
#endif

class CommandLine {
 public:
  CommandLine(int argc, char** argv) {
    for (int i = 0; i < argc; ++i) {
      if (!argv[i]) {
        continue;
      }

      if (strlen(argv[i]) < 5) {
        continue;
      }

      if (argv[i][0] != '-' || argv[1][1] != '-') {
        continue;
      }

      std::vector<std::string> v =
          absl::StrSplit(argv[i] + 2, '=', absl::SkipEmpty());
      if (v.empty()) {
        continue;
      }

      if (v.size() == 1) {
        args_[v[0]] = "";
      } else {
        args_[v[0]] = v[1];
      }
    }
  }

  std::string GetFlag(const std::string& name) {
    auto it = args_.find(name);
    if (it == args_.end()) {
      return {};
    }

    return it->second;
  }

  template <typename T>
  T GetNumericFlag(const std::string& name) {
    auto it = args_.find(name);
    if (it == args_.end()) {
      return 0;
    }

    T value;
    bool ok = absl::SimpleAtoi(it->second, &value);
    if (!ok) {
      return 0;
    }

    return value;
  }

 private:
  std::map<std::string, std::string> args_;
};

int main(int argc, char** argv) {
#ifndef NDEBUG
  ipcz::StackTrace::EnableStackTraceSymbolization(argv[0]);
#endif

  testing::InitGoogleTest(&argc, argv);
  CommandLine command_line(argc, argv);

#if defined(IPCZ_STANDALONE)
  ipcz::standalone::SetVerbosityLevel(
      command_line.GetNumericFlag<int>("verbosity"));
#endif

  ipcz::test::internal::TestClientSupport::SetCurrentProgram(argv[0]);
  std::string client_entry_point = command_line.GetFlag("run_test_client");
  if (!client_entry_point.empty()) {
    uint64_t channel_handle =
        command_line.GetNumericFlag<uint64_t>("client_channel_handle");
    ipcz::test::internal::TestClientSupport::RunEntryPoint(client_entry_point,
                                                           channel_handle);
    return 0;
  }

  return RUN_ALL_TESTS();
}
