gclient_gn_args_file = 'build/config/gclient_args.gni'
gclient_gn_args = [
  'build_with_chromium',
  'generate_location_tags',
]

vars = {
  'build_with_chromium': False,
  'generate_location_tags': False,

  'chromium_git': 'https://chromium.googlesource.com',

  'abseil_revision': '789af048b388657987c59d4da406859034fe310f',
  'build_revision': 'ebad8533842661f66b9b905e0ee9890a32f628d5',
  'buildtools_revision': 'a9bc3e283182a586998338a665c7eae17406ec54',
  'catapult_revision': '01df326efe1656bf52ef5866098a7852fb2078dc',
  'chromium_testing_revision': 'ff2d173662ccd294ddc6c7a78268de211d073dea',
  'clang_format_revision': '99803d74e35962f63a775f29477882afd4d57d94',
  'clang_revision': 'c06edd1f455183fc89e9f8c2cf745db8f564d8ea',
  'depot_tools_revision': '281edf7577c0e85f6abed10c2c36f029e864f319',
  'gn_version': 'git_revision:0153d369bbccc908f4da4993b1ba82728055926a',
  'chromium_googletest_revision': 'd1e477bfe24987837f6f8f5b09b0ceb2dbe8c3f5',
  'googletest_revision': '075810f7a20405ea09a93f68847d6e963212fa62',
  'libcxx_revision':       '79a2e924d96e2fc1e4b937c42efd08898fa472d7',
  'libcxxabi_revision':    'fd29545e5e44be805142e4b7949529355e725c09',
}

deps = {
  'build': '{chromium_git}/chromium/src/build.git@{build_revision}',
  'buildtools': '{chromium_git}/chromium/src/buildtools.git@{buildtools_revision}',

  'buildtools/clang_format/script':
      '{chromium_git}/external/github.com/llvm/llvm-project/clang/tools/clang-format.git@' +
      '{clang_format_revision}',

  'buildtools/linux64': {
    'packages': [
      {
        'package': 'gn/gn/linux-amd64',
        'version': Var('gn_version'),
      },
    ],
    'dep_type': 'cipd',
    'condition': 'host_os == "linux"',
  },

  'buildtools/mac': {
    'packages': [
      {
        'package': 'gn/gn/mac-${{arch}}',
        'version': Var('gn_version'),
      },
    ],
  },

  'buildtools/third_party/libc++/trunk':
      '{chromium_git}/external/github.com/llvm/llvm-project/libcxx.git@{libcxx_revision}',

  'buildtools/third_party/libc++abi/trunk':
      '{chromium_git}/external/github.com/llvm/llvm-project/libcxxabi.git@{libcxxabi_revision}',

  'buildtools/win': {
    'packages': [
      {
        'package': 'gn/gn/windows-amd64',
        'version': Var('gn_version'),
      }
    ],
    'dep_type': 'cipd',
    'condition': 'host_os == "win"',
  },

  'testing': '{chromium_git}/chromium/src/testing.git@{chromium_testing_revision}',
  'third_party/abseil-cpp': '{chromium_git}/chromium/src/third_party/abseil-cpp@{abseil_revision}',
  'third_party/catapult': '{chromium_git}/catapult.git@{catapult_revision}',
  'third_party/depot_tools': '{chromium_git}/chromium/tools/depot_tools.git@{depot_tools_revision}',
  'third_party/googletest':
      '{chromium_git}/chromium/src/third_party/googletest.git@{chromium_googletest_revision}',
  'third_party/googletest/src':
      '{chromium_git}/external/github.com/google/googletest@{googletest_revision}',

  'tools/clang': {
    'url': '{chromium_git}/chromium/src/tools/clang.git@{clang_revision}',
    'condition': 'not build_with_chromium',
  },
}

hooks = [
  # Download and initialize "vpython" VirtualEnv environment packages for
  # Python2. We do this before running any other hooks so that any other
  # hooks that might use vpython don't trip over unexpected issues and
  # don't run slower than they might otherwise need to.
  {
    'name': 'vpython_common',
    'pattern': '.',
    # TODO(https://crbug.com/1205263): Run this on mac/arm too once it works.
    'condition': 'not (host_os == "mac" and host_cpu == "arm64")',
    'action': [ 'vpython',
                '-vpython-spec', '.vpython',
                '-vpython-tool', 'install',
    ],
  },
  # Download and initialize "vpython" VirtualEnv environment packages for
  # Python3. We do this before running any other hooks so that any other
  # hooks that might use vpython don't trip over unexpected issues and
  # don't run slower than they might otherwise need to.
  {
    'name': 'vpython3_common',
    'pattern': '.',
    'action': [ 'vpython3',
                '-vpython-spec', '.vpython3',
                '-vpython-tool', 'install',
    ],
  },

  {
    'name': 'sysroot_x86',
    'pattern': '.',
    'condition': 'checkout_linux and (checkout_x86 or checkout_x64)',
    'action': ['python3', 'build/linux/sysroot_scripts/install-sysroot.py',
               '--arch=x86'],
  },
  {
    'name': 'sysroot_x64',
    'pattern': '.',
    'condition': 'checkout_linux and checkout_x64',
    'action': ['python3', 'build/linux/sysroot_scripts/install-sysroot.py',
               '--arch=x64'],
  },

{
    # Case-insensitivity for the Win SDK. Must run before win_toolchain below.
    'name': 'ciopfs_linux',
    'pattern': '.',
    'condition': 'checkout_win and host_os == "linux"',
    'action': [ 'python3',
                'third_party/depot_tools/download_from_google_storage.py',
                '--no_resume',
                '--no_auth',
                '--bucket', 'chromium-browser-clang/ciopfs',
                '-s', 'build/ciopfs.sha1',
    ]
  },

  {
    # Update the Windows toolchain if necessary.  Must run before 'clang' below.
    'name': 'win_toolchain',
    'pattern': '.',
    'condition': 'checkout_win',
    'action': ['python3', 'build/vs_toolchain.py', 'update', '--force'],
  },
  {
    # Update the Mac toolchain if necessary.
    'name': 'mac_toolchain',
    'pattern': '.',
    'condition': 'checkout_mac or checkout_ios',
    'action': ['python3', 'build/mac_toolchain.py'],
  },

  {
    # Update the Windows toolchain if necessary.  Must run before 'clang' below.
    'name': 'win_toolchain',
    'pattern': '.',
    'condition': 'checkout_win',
    'action': ['python3', 'build/vs_toolchain.py', 'update', '--force'],
  },
  {
    # Update the Mac toolchain if necessary.
    'name': 'mac_toolchain',
    'pattern': '.',
    'condition': 'checkout_mac or checkout_ios',
    'action': ['python3', 'build/mac_toolchain.py'],
  },

  {
    # Update the prebuilt clang toolchain.
    # Note: On Win, this should run after win_toolchain, as it may use it.
    'name': 'clang',
    'pattern': '.',
    'action': ['python3', 'tools/clang/scripts/update.py'],
  },

  # Update LASTCHANGE.
  {
    'name': 'lastchange',
    'pattern': '.',
    'action': ['python3', 'build/util/lastchange.py', '-o',
               'build/util/LASTCHANGE']
  },

  # Don't let the DEPS'd-in depot_tools self-update.
  {
    'name': 'disable_depot_tools_selfupdate',
    'pattern': '.',
    'action': [
      'python3',
      'third_party/depot_tools/update_depot_tools_toggle.py',
      '--disable',
    ],
  },

  # Pull clang-format binaries using checked-in hashes.
  {
    'name': 'clang_format_win',
    'pattern': '.',
    'condition': 'host_os == "win"',
    'action': [ 'python3',
                'third_party/depot_tools/download_from_google_storage.py',
                '--no_resume',
                '--no_auth',
                '--bucket', 'chromium-clang-format',
                '-s', 'buildtools/win/clang-format.exe.sha1',
    ],
  },
  {
    'name': 'clang_format_mac',
    'pattern': '.',
    'condition': 'host_os == "mac"',
    'action': [ 'python3',
                'third_party/depot_tools/download_from_google_storage.py',
                '--no_resume',
                '--no_auth',
                '--bucket', 'chromium-clang-format',
                '-s', 'buildtools/mac/clang-format.sha1',
    ],
  },
  {
    'name': 'clang_format_linux',
    'pattern': '.',
    'condition': 'host_os == "linux"',
    'action': [ 'python3',
                'third_party/depot_tools/download_from_google_storage.py',
                '--no_resume',
                '--no_auth',
                '--bucket', 'chromium-clang-format',
                '-s', 'buildtools/linux64/clang-format.sha1',
    ],
  },

]

