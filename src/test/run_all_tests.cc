// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#ifndef NDEBUG
#include "debug/stack_trace.h"
#endif

#include "test/test_client.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/flags/flag.h"
#include "third_party/abseil-cpp/absl/flags/parse.h"

ABSL_FLAG(std::string, run_test_client, "", "test client entry point name");
ABSL_FLAG(uint64_t, client_channel_handle, 0, "client channel handle");

int main(int argc, char** argv) {
#ifndef NDEBUG
  ipcz::debug::StackTrace::EnableStackTraceSymbolization(argv[0]);
#endif

  testing::InitGoogleTest(&argc, argv);
  absl::ParseCommandLine(argc, argv);

  ipcz::test::internal::TestClientSupport::SetCurrentProgram(argv[0]);
  std::string client_entry_point = absl::GetFlag(FLAGS_run_test_client);
  if (!client_entry_point.empty()) {
    uint64_t channel_handle = absl::GetFlag(FLAGS_client_channel_handle);
    ipcz::test::internal::TestClientSupport::RunEntryPoint(client_entry_point,
                                                           channel_handle);
    return 0;
  }

  return RUN_ALL_TESTS();
}
