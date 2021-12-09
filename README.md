ipcz
====

Overview
----

ipcz is a minimalist API for fast, lightweight, cross-platform IPC primitives
called *portals*. Portals extensively leverage shared memory resources among
connected processes to enable efficient packet routing and delivery, with
coalesced signaling to reduce overhead from redundant system calls and context
switches when under heavy IPC load.

Performance and internal versioning considerations, as well as public API
design, are inspired by several years of experience hacking on and around
Chromium's Mojo IPC system. ipcz is designed to be a simpler and more efficient
replacement for Mojo, but it is also sufficiently capable to support the
re-implementation of Mojo itself as a thin transitional wrapper around ipcz.

Unlike Mojo, ipcz does not make a distinction between primitives which carry
framed message data vs primitives which carry large streamed data payloads.
Instead, portals are flexible enough to cover both use cases efficiently with
minor configuration tweaks. Additionally unlike Mojo, ipcz does not define any
API surface to manipulate shared memory objects as first-class primitives.

Project Structure & API
----

Mojo has been frequently considered as an option for IPC in projects external to
Chromium, but its dependence on unstable and non-exported Chromium APIs makes it
an exceptionally difficult dependency to integrate into another project. In
light of this, it is an explicit goal to build and maintain ipcz as a viable
standalone library project outside of the Chromium tree.

There is very little redundancy between ipcz code and Chromium code, and no
dependencies on Chromium code. In fact there are no large external dependencies
of any kind, making ipcz relatively easy for external projects to consume.

ipcz is designed to be consumed interchangeably as either a static library or a
shared library, with versioning considerations built into all of its internal
communications. The C ABI exported by ipcz is designed to remain stable and
extensible so integration into a large project like Chromium is not burdened by
frequent refactorings or superficial churn.

ipcz does NOT define any kind of structured messaging protocol. Any RPC, IDL, or
related code-generation business is fundamentally out of scope for ipcz, but
ipcz is a perfectly suitable medium through which to transmit such protocols.

Finally, ipcz itself does not do any actual I/O! Its interaction with
platform-specific APIs is generally limited to internal shared memory allocation
and manipulation, and simple synchronization primitives. In order for ipcz to
perform I/O between processes, it relies on the embedding application to provide
a lightweight driver implementation. Two reference drivers are provided in
`src/drivers`: one for real IPC which supports interconnection of ipcz nodes
living in different processes, and a single-process driver which performs all
communication synchronously (and re-entrantly) between nodes in the same
process. Both reference driver implementations are relatively simple.

Why Exist?
----

Mostly for Chromium. Most applications probably don't need or want the massive
number of independent and transferrable communication endpoints that Chromium is
built with and which ipcz is designed to facilitate. For simpler applications
it may be sufficient to use sockets or some other equivalent for your favorite
platform.

However, many applications (particularly on Chrome OS) need to communicate with
Chrome or with other applications built upon the Chromium project, and having
one way to do IPC across such applications has plenty of security and code
health benefits.

Of course ipcz is also just fine for smaller use cases, so its seamless
scalability to larger and more complex systems along with its uniform API across
platforms may render it a suitable standard for large collections of
interconnected software.

