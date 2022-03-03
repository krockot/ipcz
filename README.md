ipcz
====

ipcz is a C library for interprocess communication (IPC).

Setup
----
To set up a new local repository, first install
[depot\_tools](https://commondatastorage.googleapis.com/chrome-infra-docs/flat/depot_tools/docs/html/depot_tools_tutorial.html#_setting_up)
and make sure it's in your `PATH`.

Then from within the repository root:

```
cp .gclient-default .gclient
gclient sync
```

When updating a local copy of the repository, it's a good idea to rerun
`gclient sync` to ensure that all external dependencies are up-to-date.

Build
----

ipcz uses GN for builds. This is provided by the `depot_tools` installation.

To create a new build configuration, first create a directory for it. For
example on Linux or macOS:

```
mkdir -p out/Debug
```

Then run `gn args` to create and edit the build configuration:

```
gn args out/Debug
```

For a typical debug build the contents may be as simple as:

```
is_debug = true
```

Now targets can be built:

```
ninja -C out/Debug ipcz_tests

# Hope they all pass!
./ipcz_tests
```

Usage
----

ipcz may be statically linked into a project, or it may be consumed as a shared
library. A shared library can be built with the `ipcz_shared` target.

ipcz is meant to be consumed exclusively through the C ABI defined in
[`include/ipcz/ipcz.h`](include/ipcz/ipcz.h). Applications populate an `IpczAPI`
structure by calling `IpczGetAPI()`, the library's only exported symbol. From
there they can create and connect nodes and establish portals for higher-level
communication.

Applications *must* provide each node with an implementation of the
`IpczDriver` structure, implementing straightforward operations to create,
serialize, deserialize, and use I/O transport mechanisms and shared memory
regions. See [reference drivers](src/reference_drivers) for examples.

Design
----

Some extensive coverage of ipcz design details can be found
[here](https://docs.google.com/document/d/1i49DF2af4JDspE1fTXuPrUvChQcqDChdHH6nx4xiyoY/edit?resourcekey=0-t_viq9NAbGb5kr_ni9scTA#heading=h.rlzi4jxw96rk).

